"""Run the compiled mapping service and XMB actions against mocked PSP APIs.

The Bluetooth captures are optional, as in the other firmware execution tests.
No firmware code or controller addresses are shipped with the plugin.
"""
from pathlib import Path
import struct
import subprocess
import sys
import unittest

from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_LITTLE_ENDIAN, UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import machine, SYMBOLS
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from psp_exports import function_exports, function_nid

BASE = 0x0A572000
CAPTURE = Path("pspgobt/bluetooth_plugin_module-04FE4C4D.text.bin")
RET = 0x0A700F00
ARGS = [reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3]
DEFAULT = bytes(range(17)) + bytes(7)
UI_ABI = 4
VTABLE = 0x1FD58
CLOSE_SLOT = 0x8C
ENTRY = "Controller Mapping (then press Triangle)"
HINT = "After selecting, press Triangle to continue"
MAIN_ROWS, BUTTON_CHOICES = 24, 19
ADDRESS = bytes([1, 2, 3, 4, 5, 6])
PATH = b"ef0:/seplugins/pspgobt/mappings.cfg"


def symbols(path):
    return {f[2]: int(f[0], 16) for line in subprocess.check_output(["psp-nm", path], text=True).splitlines()
            if len(f := line.split()) == 3}


def read32(cpu, pointer):
    return struct.unpack("<I", cpu.mem_read(pointer, 4))[0]


def write32(cpu, pointer, value):
    cpu.mem_write(pointer, struct.pack("<I", value))


def string(cpu, pointer):
    return bytes(cpu.mem_read(pointer, 96)).split(b"\0", 1)[0]


def invoke(cpu, pointer, *args):
    cpu.reg_write(reg.UC_MIPS_REG_SP, 0x0A60F000)
    cpu.reg_write(reg.UC_MIPS_REG_RA, RET)
    for r, value in zip(ARGS, args):
        cpu.reg_write(r, value)
    cpu.emu_start(pointer, RET, count=1000000)
    if cpu.reg_read(reg.UC_MIPS_REG_PC) != RET:
        raise AssertionError(f"Execution did not return: {cpu.reg_read(reg.UC_MIPS_REG_PC):08x}")
    return cpu.reg_read(reg.UC_MIPS_REG_V0)


class MappingStorage(unittest.TestCase):
    def test_mapping_gateway_is_exported_as_a_syscall_in_built_prx(self):
        exports = function_exports(Path("pspgobt_probe.prx").read_bytes())
        matches = [(address, flags) for library, nid, address, flags in exports
                   if library == "PSPGoBT_trace" and nid == function_nid("pspgobtMappingService")]
        self.assertEqual(matches, [(SYMBOLS["pspgobtMappingService"], 0x4001)])

    def setUp(self):
        self.cpu = machine()
        self.files, self.opened, self.saved = {}, {}, []
        self.fail_write = self.fail_commit = False
        self.fail_rollback = False
        self.address = ADDRESS
        self.buffer = 0x0A600000
        self.k1 = 0x12345678
        self.waits = self.signals = 0
        self.cpu.reg_write(reg.UC_MIPS_REG_K1, self.k1)
        self.cpu.hook_add(UC_HOOK_CODE, self.mock)

    def mock(self, m, pc, _, __):
        a, b, c = [m.reg_read(r) for r in ARGS[:3]]
        returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1, SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                   SYMBOLS["sceKernelCreateSema"]: 55, SYMBOLS["sceKernelDevkitVersion"]: 0x06060010,
                   SYMBOLS["sceKernelDcacheWritebackInvalidateAll"]: 0, SYMBOLS["sceKernelIcacheInvalidateAll"]: 0}
        if pc == SYMBOLS["sceKernelWaitSema"]:
            self.waits += 1
            returns[pc] = 0
        if pc == SYMBOLS["sceKernelSignalSema"]:
            self.signals += 1
            returns[pc] = 0
        if pc == SYMBOLS.get("strcmp"):
            returns[pc] = int(string(m, a) != string(m, b))
        if pc in (SYMBOLS["gobt_input_bridge_mapping_address"], SYMBOLS["gobt_input_bridge_paired_address"]):
            if a == 1:
                m.mem_write(b, self.address)
            returns[pc] = int(a == 1)
        if pc == SYMBOLS["sceIoOpen"]:
            name = string(m, a)
            if b & 2:
                self.files[name] = b""
            if name in self.files:
                fd = 10 + len(self.opened)
                self.opened[fd] = name
                returns[pc] = fd
            else:
                returns[pc] = -1
        if pc == SYMBOLS["sceIoRead"]:
            data = self.files[self.opened[a]][:c]
            m.mem_write(b, data)
            returns[pc] = len(data)
        if pc == SYMBOLS["sceIoWrite"]:
            data = bytes(m.mem_read(b, c))
            if self.fail_write:
                data = data[:c // 2]
            self.files[self.opened[a]] = data
            returns[pc] = len(data)
        if pc == SYMBOLS["sceIoClose"]:
            returns[pc] = 0
        if pc == SYMBOLS["sceIoRemove"]:
            returns[pc] = 0 if self.files.pop(string(m, a), None) is not None else -1
        if pc == SYMBOLS["sceIoRename"]:
            old, new = string(m, a), string(m, b)
            result = -1
            if (old in self.files and new not in self.files and not (self.fail_commit and old.endswith(b".tmp"))
                    and not (self.fail_rollback and old.endswith(b".bak"))):
                self.files[new] = self.files.pop(old)
                result = 0
            returns[pc] = result
        for name in ("memcpy", "memset", "memcmp"):
            if pc != SYMBOLS.get(name):
                continue
            if name == "memcpy":
                m.mem_write(a, bytes(m.mem_read(b, c)))
                returns[pc] = a
            elif name == "memset":
                m.mem_write(a, bytes([b & 255]) * c)
                returns[pc] = a
            else:
                x, y = bytes(m.mem_read(a, c)), bytes(m.mem_read(b, c))
                returns[pc] = (x > y) - (x < y)
        if pc in returns:
            m.reg_write(reg.UC_MIPS_REG_V0, returns[pc] & 0xFFFFFFFF)
            m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

    def call(self, name, *args):
        return invoke(self.cpu, SYMBOLS[name], *args)

    def request(self, command, mapping=DEFAULT, address=ADDRESS):
        waits = self.waits
        self.cpu.mem_write(self.buffer, struct.pack("<II", 1, 1) + address + bytes(2) + mapping)
        result = self.call("pspgobtMappingService", command, self.buffer)
        self.assertEqual(self.cpu.reg_read(reg.UC_MIPS_REG_K1), self.k1)
        self.assertEqual(self.waits, self.signals)
        if command == 1:
            self.assertEqual(self.waits, waits, "Opening a menu must not wait on a semaphore")
        return result, bytes(self.cpu.mem_read(self.buffer + 16, 24))

    def test_save_restore_reconnect_and_separate_addresses(self):
        self.assertEqual(self.call("gobt_mapping_start"), 0)
        self.assertEqual(self.request(1), (0, DEFAULT))
        changed = bytearray(DEFAULT)
        changed[14] = 13
        self.assertEqual(self.request(2, bytes(changed))[0], 0)
        primary = self.files[PATH]
        # A second controller never inherits the first one's profile.
        self.address = bytes([9, 8, 7, 6, 5, 4])
        self.assertEqual(self.request(1)[1], DEFAULT)
        self.assertEqual(self.request(2, address=ADDRESS)[0], 0xFFFFFFFF)
        self.address = ADDRESS
        # Reboot: discard RAM, reload the exact persisted bytes.
        self.cpu = machine()
        self.cpu.reg_write(reg.UC_MIPS_REG_K1, self.k1)
        self.cpu.hook_add(UC_HOOK_CODE, self.mock)
        self.assertEqual(self.call("gobt_mapping_start"), 0)
        self.assertEqual(self.request(1)[1], changed)
        self.cpu.mem_write(self.buffer, ADDRESS)
        self.call("gobt_mapping_select", self.buffer)
        self.cpu.mem_write(self.buffer + 64, struct.pack("<I6B", 1 << 14, 10, 20, 30, 40, 0, 0))
        self.call("gobt_mapping_apply", self.buffer + 64, self.buffer + 96)
        self.assertEqual(read32(self.cpu, self.buffer + 96), 1 << 13)
        self.assertEqual(self.files[PATH], primary)

    def test_failed_write_and_commit_preserve_profile(self):
        self.call("gobt_mapping_start")
        self.assertEqual(self.request(2)[0], 0)
        primary = self.files[PATH]
        changed = bytearray(DEFAULT); changed[14] = 13
        for failure in ("fail_write", "fail_commit"):
            setattr(self, failure, True)
            self.assertEqual(self.request(2, bytes(changed))[0], 0xFFFFFFFF)
            self.assertEqual(self.request(1)[1], DEFAULT)
            self.assertEqual(self.files[PATH], primary)
            setattr(self, failure, False)

    def test_backup_recovery_survives_failed_commit(self):
        self.call("gobt_mapping_start")
        changed = bytearray(DEFAULT); changed[14] = 13
        self.request(2, bytes(changed))
        backup = self.files.pop(PATH)
        self.files[PATH + b".bak"] = backup
        self.files[PATH] = b"damaged"
        self.cpu = machine()
        self.cpu.reg_write(reg.UC_MIPS_REG_K1, self.k1)
        self.cpu.hook_add(UC_HOOK_CODE, self.mock)
        self.call("gobt_mapping_start")
        self.assertEqual(self.request(1)[1], changed)
        self.fail_commit = True
        self.assertEqual(self.request(2)[0], 0xFFFFFFFF)
        self.assertEqual(self.files[PATH + b".bak"], backup)

    def test_invalid_mapping_leaves_storage_untouched(self):
        self.call("gobt_mapping_start")
        changed = bytearray(DEFAULT); changed[14] = 17
        self.assertEqual(self.request(2, bytes(changed))[0], 0xFFFFFFFF)
        self.assertNotIn(PATH, self.files)
        self.assertEqual(self.request(1)[1], DEFAULT)

    def test_failed_rollback_keeps_recovery_on_next_save(self):
        self.call("gobt_mapping_start")
        self.request(2)
        primary = self.files[PATH]
        self.fail_commit = self.fail_rollback = True
        self.assertEqual(self.request(2)[0], 0xFFFFFFFF)
        self.assertEqual(self.files[PATH + b".bak"], primary)
        self.assertEqual(self.request(2)[0], 0xFFFFFFFF)
        self.assertEqual(self.files[PATH + b".bak"], primary)

    @unittest.skipUnless(CAPTURE.exists(), "local Bluetooth firmware capture not installed")
    def test_kernel_installs_only_verified_user_hooks(self):
        self.cpu.mem_map(0x0A570000, 0x40000)
        self.cpu.mem_write(BASE, CAPTURE.read_bytes())
        original_lookup = bytes(self.cpu.mem_read(BASE + 0x11E4, 8))
        patch = (UI_ABI, BASE, 0x20C68, 0x0A700800, 0x0A700840)
        self.cpu.mem_write(self.buffer, struct.pack("<5I", *patch))
        self.assertEqual(self.call("pspgobtMappingService", 4, self.buffer), 0)
        self.assertEqual(read32(self.cpu, BASE + 0x10A80), 0x08000000 | ((patch[3] >> 2) & 0x03FFFFFF))
        self.assertEqual(read32(self.cpu, BASE + 0x10A84), 0)
        # The generic option path jumped straight to 0x10A84; it now replays
        # that displaced load in its own stub instead of landing on the nop.
        self.assertEqual(read32(self.cpu, BASE + 0x10BB8), 0x08000000 | ((patch[4] >> 2) & 0x03FFFFFF))
        # Sony's close completion is left alone: reopening from it overlapped
        # the page's own close on hardware.
        self.assertEqual(read32(self.cpu, BASE + VTABLE + CLOSE_SLOT), BASE + 0x91CC)
        self.assertEqual(bytes(self.cpu.mem_read(BASE + 0x11E4, 8)), original_lookup)
        self.assertEqual(self.cpu.reg_read(reg.UC_MIPS_REG_K1), self.k1)
        # A second install, or an altered original, fails without touching it.
        self.assertEqual(self.call("pspgobtMappingService", 4, self.buffer), 0xFFFFFFFF)
        self.assertEqual(self.cpu.reg_read(reg.UC_MIPS_REG_K1), self.k1)

    def test_shutdown_rejects_menu_requests(self):
        self.call("gobt_mapping_start")
        self.call("gobt_mapping_shutdown")
        self.assertEqual(self.request(1)[0], 0xFFFFFFFF)
        self.assertEqual(self.request(2)[0], 0xFFFFFFFF)
        self.assertEqual(self.files, {})

    def test_address_snapshot_never_queries_bluetooth_or_enters_transport_callback(self):
        cpu = machine()
        cpu.mem_map(0x08200000, 0x10000)
        state, output = 0x88201000, 0x0A600000
        write32(cpu, SYMBOLS["installed"], 1)
        write32(cpu, SYMBOLS["paired_records"], state)
        write32(cpu, SYMBOLS["callback_depth"], 9)
        record = struct.pack("<HHH", 1, 0x0402, 0) + ADDRESS + bytes(4)
        cpu.mem_write((state + 0x24) & 0x1FFFFFFF, record)

        def snapshot_mock(m, pc, _, __):
            a, b, size = [m.reg_read(r) for r in ARGS[:3]]
            returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1, SYMBOLS["sceKernelCpuResumeIntr"]: 0}
            if pc == SYMBOLS["memcpy"]:
                physical = lambda p: p & 0x1FFFFFFF if p >= 0x80000000 else p
                m.mem_write(physical(a), bytes(m.mem_read(physical(b), size)))
                returns[pc] = a
            if pc in [SYMBOLS[name] for name in ("classify", "callback_enter", "callback_leave", "sceKernelWaitSema")]:
                self.fail("The mapping address snapshot entered a potentially blocking transport path")
            if pc in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[pc])
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, snapshot_mock)
        self.assertEqual(invoke(cpu, SYMBOLS["gobt_input_bridge_mapping_address"], 1, output), 1)
        self.assertEqual(bytes(cpu.mem_read(output, 6)), ADDRESS)
        self.assertEqual(read32(cpu, SYMBOLS["callback_depth"]), 9)
        write32(cpu, SYMBOLS["suspended"], 1)
        self.assertEqual(invoke(cpu, SYMBOLS["gobt_input_bridge_mapping_address"], 1, output), 0)

    @unittest.skipUnless(CAPTURE.exists(), "local Bluetooth firmware capture not installed")
    def test_helper_loads_outside_module_callback_and_rechecks_owner(self):
        self.cpu.mem_map(0x0A570000, 0x40000)
        self.cpu.mem_write(BASE, CAPTURE.read_bytes())
        module, worker_args = 0x0A600400, 0x0A606000
        self.cpu.mem_write(module + 8, b"bluetooth_plugin_module\0")
        write32(self.cpu, module + 44, 7)
        self.cpu.mem_write(module + 108, struct.pack("<II", BASE, 0x20C68))
        created, loaded, started, exits, unloaded, stopped = [], [], [], [], [], []
        alive, stubs = [True], []

        def loader_mock(m, pc, _, __):
            a, b, c, d = [m.reg_read(r) for r in ARGS]
            returns = {}
            if pc == SYMBOLS["sctrlHENMakeSyscallStub"]:
                self.assertEqual(a, SYMBOLS["pspgobtMappingService"])
                # ARK can create a valid syscall stub only for a registered
                # syscall export. Do not assume every function is callable.
                exports = function_exports(Path("pspgobt_probe.prx").read_bytes())
                registered = any(address == a and flags & 0x4000 for _, _, address, flags in exports)
                stubs.append(a)
                returns[pc] = 0x0A700A00 if registered else 0
            elif pc == SYMBOLS["sceKernelCreateThread"]:
                self.assertEqual(string(m, a), b"PSPGoBTMappingUI")
                created.append(b)
                returns[pc] = 77
            elif pc == SYMBOLS["sceKernelStartThread"]:
                self.assertEqual((a, b), (77, 24))
                m.mem_write(worker_args, bytes(m.mem_read(c, b)))
                returns[pc] = 0
            elif pc == SYMBOLS["sceKernelFindModuleByUID"]:
                self.assertEqual(a, 7)
                returns[pc] = module if alive[0] else 0
            elif pc == SYMBOLS["sceKernelLoadModule"]:
                loaded.append(string(m, a))
                returns[pc] = 88
            elif pc == SYMBOLS["sceKernelStartModule"]:
                self.assertEqual((a, b), (88, 16))
                started.append(struct.unpack("<4I", m.mem_read(c, 16)))
                write32(m, d, 0)
                returns[pc] = 0
            elif pc == SYMBOLS["sceKernelUnloadModule"]:
                unloaded.append(a)
                returns[pc] = 0
            elif pc == SYMBOLS["sceKernelStopModule"]:
                stopped.append(a)
                returns[pc] = 0
            elif pc == SYMBOLS["sceKernelExitDeleteThread"]:
                exits.append(a)
                returns[pc] = 0
            if pc in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[pc])
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        self.cpu.hook_add(UC_HOOK_CODE, loader_mock)
        self.call("gobt_mapping_install", module, 0x06060010)
        self.call("gobt_mapping_install", module, 0x06060010)
        self.assertEqual(len(created), 2)
        self.assertEqual(stubs, [SYMBOLS["pspgobtMappingService"]])  # One syscall stub, reused.
        self.assertEqual(loaded, [])  # Loader callback has already returned.
        invoke(self.cpu, created[0], 24, worker_args)
        self.assertEqual(loaded, [b"ef0:/seplugins/pspgobt/mapping_ui.prx"])
        self.assertEqual(started, [(UI_ABI, BASE, 0x20C68, 0x0A700A00)])
        self.assertEqual(exits, [0])
        self.assertEqual(unloaded, [])
        # Same Bluetooth instance again: its hooks still use this helper.
        invoke(self.cpu, created[0], 24, worker_args)
        self.assertEqual((len(loaded), stopped, unloaded), (1, [], []))
        # Leaving Settings unloads Bluetooth and the image holding our hooks.
        # The next visit retires the stale helper before loading a new one.
        alive[0] = False
        invoke(self.cpu, created[0], 24, worker_args)
        self.assertEqual((len(loaded), stopped, unloaded), (1, [88], [88]))
        alive[0] = True
        invoke(self.cpu, created[0], 24, worker_args)
        self.assertEqual(len(loaded), 2)
        self.assertEqual(len(exits), 4)


@unittest.skipUnless(CAPTURE.exists(), "local Bluetooth firmware capture not installed")
class MappingMenu(unittest.TestCase):
    def setUp(self):
        self.cpu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN)
        self.ui_base = getattr(self, "UI_BASE", 0)
        self.cpu.mem_map(self.ui_base, 0x10000)
        elf = Path("ui/mapping_ui.elf").read_bytes()
        section_offset = struct.unpack_from("<I", elf, 32)[0]
        section_size, section_count = struct.unpack_from("<HH", elf, 46)
        for i in range(section_count):
            _, kind, flags, addr, off, size, *_ = struct.unpack_from("<10I", elf, section_offset + i * section_size)
            if flags & 2 and kind != 8 and size:
                self.cpu.mem_write(self.ui_base + addr, elf[off:off + size])
        if self.ui_base:
            self.assertEqual(self.ui_base & 0xffff, 0)
            sections = [struct.unpack_from("<10I", elf, section_offset + i * section_size) for i in range(section_count)]
            for section in sections:
                if section[1] != 9 or not sections[section[7]][2] & 2:
                    continue
                for offset in range(section[4], section[4] + section[5], section[9]):
                    target, info = struct.unpack_from("<II", elf, offset)
                    instruction = read32(self.cpu, self.ui_base + target)
                    kind = info & 255
                    if kind == 2:
                        instruction += self.ui_base
                    elif kind == 4:
                        instruction = (instruction & 0xfc000000) | (((instruction & 0x03ffffff) + (self.ui_base >> 2)) & 0x03ffffff)
                    elif kind == 5:
                        instruction = (instruction & 0xffff0000) | (((instruction & 0xffff) + (self.ui_base >> 16)) & 0xffff)
                    elif kind != 6:
                        self.fail(f"Unsupported helper relocation {kind}")
                    write32(self.cpu, self.ui_base + target, instruction)
        self.symbols = {name: value + self.ui_base for name, value in symbols("ui/mapping_ui.elf").items()}
        self.cpu.mem_map(0x0A570000, 0x40000)
        self.cpu.mem_write(BASE, CAPTURE.read_bytes())
        self.cpu.mem_map(0x0A600000, 0x10000)
        self.cpu.mem_map(0x0A700000, 0x1000)
        self.menu, self.widget, self.page, self.parent = 0x0A600000, 0x0A600100, 0x0A600200, 0x0A600300
        self.manager, self.order, self.profiles = 0x0A600400, 0x0A600500, 0x0A600600
        self.menu_table, self.widget_table, self.page_table = 0x0A601000, 0x0A602000, 0x0A603000
        self.service, self.add, self.index, self.bind, self.show = [0x0A700000 + i * 0x100 for i in range(5)]
        for pointer, value in [(BASE + 0x20D5C, self.manager), (BASE + 0x20D70, self.widget), (BASE + 0x20D78, self.page),
                               (self.menu, self.menu_table), (self.widget, self.widget_table), (self.page, self.page_table),
                               (self.menu_table + 0x180, self.add), (self.widget_table + 0x194, self.index),
                               (self.page_table + 0x168, self.bind), (self.menu_table + 0x48, self.show),
                               (self.manager + 4, self.order), (self.manager + 8, 1),
                               (self.manager + 20, 9), (self.manager + 24, self.profiles), (self.manager + 28, 0),
                               (self.profiles - 4, 9), (self.order, 1)]:
            write32(self.cpu, pointer, value)
        self.cpu.mem_write(self.profiles + 44, struct.pack("<II", 1, 9))
        write32(self.cpu, self.symbols["bt_base"], BASE)
        write32(self.cpu, self.symbols["service"], self.service)
        self.rows, self.saves, self.selections, self.binds = [], [], [], []
        self.supported, self.fail_save = True, False
        self.native = False
        self.emulate_close, self.completions, self.opens, self.notes = True, [], [], []
        self.completion_flags = self.completion_page = 0
        self.generic = False
        self.mock_hook = self.cpu.hook_add(UC_HOOK_CODE, self.mock)

    def mock(self, m, pc, _, __):
        a, b, c, d = [m.reg_read(r) for r in ARGS]
        result = None
        if pc == self.service:
            result = 0
            if a == 1:
                if not self.supported:
                    result = -1
                else:
                    m.mem_write(b + 8, ADDRESS + bytes(2) + DEFAULT)
            elif a == 2:
                self.saves.append(bytes(m.mem_read(b, 40)))
                result = -1 if self.fail_save else 0
            elif a == 5:
                self.notes.append(struct.unpack("<4I", m.mem_read(b, 16)))
            elif a == 4:
                abi, base, size, append, bypass = struct.unpack("<5I", m.mem_read(b, 20))
                self.assertEqual((abi, base, size), (UI_ABI, BASE, 0x20C68))
                m.mem_write(base + 0x10A80, struct.pack("<II", 0x08000000 | ((append >> 2) & 0x03FFFFFF), 0))
                write32(m, base + 0x10BB8, 0x08000000 | ((bypass >> 2) & 0x03FFFFFF))
        elif pc == BASE + 0x91CC and self.emulate_close:
            # Native completion: rows cleared, owner notified, page detached
            # and flags reset (unless a test leaves the menu non-idle).
            self.completions.append(a)
            if a == self.menu:
                self.rows.clear()
                write32(m, self.menu + 0x2C, self.completion_flags)
                write32(m, self.menu + 0x08, self.completion_page)
            result = 0x5A5A
        elif pc == BASE + 0x9DD4 and self.emulate_close:
            # Native open builds through the hooked device builder.
            self.opens.append((a, b, string(m, c), d))
            if self.native:  # Run Sony's complete device builder.
                m.reg_write(reg.UC_MIPS_REG_PC, BASE + 0x108DC)
                return
            m.reg_write(reg.UC_MIPS_REG_A1, self.widget)
            m.reg_write(reg.UC_MIPS_REG_A2, b)
            m.reg_write(reg.UC_MIPS_REG_PC, self.symbols["mapping_append"])
            return
        elif pc == BASE + 0x15CB4:
            length = 0
            while bytes(m.mem_read(b + length * 2, 2)) != bytes(2):
                length += 1
            m.mem_write(a, struct.pack("<II", b, length))
            result = a
        elif pc == BASE + 0x16CC0:
            pointer, length, _, callback, value = struct.unpack("<5I", m.mem_read(b, 20))
            label = bytes(m.mem_read(pointer, length * 2)).decode("utf-16le")
            self.rows.append((label, callback, value))
            entry = 0x0A604000 + (len(self.rows) - 1) * 20
            m.mem_write(entry, bytes(m.mem_read(b, 20)))
            write32(m, self.menu + 0x4C, 0x0A604000)
            write32(m, self.menu + 0x50, len(self.rows))
            result = 0
        elif pc == BASE + 0x158A0:
            result = 0
        elif pc == self.index:
            result = 0
        elif pc == self.add:
            key = string(m, b)
            if key.startswith(b"pspgobt_"):
                index = int(key[8:10])
                label = bytes(m.mem_read(self.symbols["labels"] + index * 192, 192)).decode("utf-16le").split("\0", 1)[0]
            else:
                label = key.decode()
            self.rows.append((label, c, d))
            # Native callback dispatch consumes 20-byte entries at menu+0x4c.
            entry = 0x0A604000 + (len(self.rows) - 1) * 20
            m.mem_write(entry, struct.pack("<5I", 0, 0, 0, c, d))
            write32(m, self.menu + 0x4C, 0x0A604000)
            write32(m, self.menu + 0x50, len(self.rows))
            result = 0
        elif pc == BASE + 0x67D0:
            self.rows.clear()
            result = 0
        elif pc == self.bind:
            name = b"page_bt_config_option" if self.generic else b"page_bt_config_registered_option"
            self.assertEqual((a, b, string(m, c), d), (self.page, self.parent, name, 0))
            self.binds.append(name)
            result = 0
        elif pc == self.show:
            self.assertEqual((a, b), (self.menu, self.page))
            self.selections.append(read32(m, self.menu + 0x44))
            result = 0
        elif pc == self.symbols["sceKernelDcacheWritebackAll"]:
            result = 0
        elif self.native:
            if pc == BASE + 0x15F00:
                m.mem_write(a, struct.pack("<II", b, 0))
                result = a
            elif pc == 0x0A700500:
                # Class name check: true only for the generic option list.
                self.assertEqual(string(m, b), b"mlist_bt_config_option")
                result = int(self.generic)
            elif pc in (BASE + 0x157D8, BASE + 0x14818, BASE + 0x14858, 0x0A700600):
                result = 0
        if result is not None:
            m.reg_write(reg.UC_MIPS_REG_V0, result & 0xFFFFFFFF)
            m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

    def call(self, name, *args):
        return invoke(self.cpu, self.symbols[name], *args)

    def dispatch(self, index):
        # Execute Sony's actual callback dispatcher, checking its ABI rather
        # than assuming that the native event passes the menu/data as expected.
        event, source = 0x0A605000, 0x0A605100
        write32(self.cpu, event + 24, source)
        write32(self.cpu, source + 12, index)
        invoke(self.cpu, BASE + 0x6810, self.menu, event)

    def triangle(self):
        """The user's Triangle press: the native close cleared the old rows,
        then Sony's builder (or, at mock level, its hooked tail) runs."""
        self.rows.clear()
        if self.native:
            invoke(self.cpu, BASE + 0x108DC, self.menu, self.parent)
        else:
            self.call("mapping_append", self.menu, self.widget, self.parent)

    def select(self, index):
        # Selecting never rebuilds or reopens while the native close runs.
        before, binds = list(self.rows), len(self.binds)
        self.dispatch(index)
        self.assertEqual((self.rows, len(self.binds)), (before, binds))
        self.triangle()

    def labels(self):
        return [row[0] for row in self.rows]

    def open(self):
        self.triangle()
        self.assertEqual(self.labels(), [ENTRY])
        self.select(0)
        self.assertEqual(len(self.rows), MAIN_ROWS)
        self.assertEqual(self.rows[-1][:2], (HINT, 0))  # Guidance, not selectable.

    def test_open_change_button_and_stick_save(self):
        self.open()
        self.assertIn("Cross / B: Cross", self.labels())
        self.select(14)
        self.assertEqual(len(self.rows), BUTTON_CHOICES)
        self.assertEqual(read32(self.cpu, self.menu + 0x44), 14)  # Current target highlighted.
        self.select(13)
        self.assertIn("Cross / B: Circle", self.labels())
        self.assertEqual(read32(self.cpu, self.menu + 0x44), 14)  # Back on the edited row.
        self.select(17)
        self.select(1)
        self.assertIn("PSP analog: Right stick", self.labels())
        self.select(20)
        self.assertEqual(len(self.saves), 1)
        self.assertEqual(self.saves[0][8:14], ADDRESS)
        self.assertEqual(self.saves[0][16 + 14], 13)
        self.assertEqual(self.saves[0][16 + 17], 1)
        # Saved: the next Triangle is the ordinary device menu.
        self.assertEqual(self.labels(), [ENTRY])

    def test_cancel_and_defaults_and_error(self):
        self.open()
        self.select(14); self.select(13)
        self.select(21)
        self.assertIn("Cross / B: Cross", self.labels())
        self.select(22)
        self.assertFalse(self.saves)
        self.assertEqual(self.labels(), [ENTRY])
        self.open()
        self.fail_save = True
        self.select(20)
        self.assertIn("Save failed", self.rows[-1][0])
        self.assertEqual(self.rows[-1][1], 0)  # Error text is not selectable.

    def test_pending_screen_is_shown_once_by_the_next_triangle(self):
        self.triangle()
        self.dispatch(0)
        # Leaving with Circle and returning later still continues the edit.
        self.triangle()
        self.assertEqual(len(self.rows), MAIN_ROWS)
        self.assertEqual(self.notes[-1][:2], (8, 1))
        self.triangle()  # Consumed: closing without a selection ends editing.
        self.assertEqual(self.labels(), [ENTRY])

    def test_triangle_on_other_controller_discards_pending_editor(self):
        self.triangle()
        self.dispatch(0)
        write32(self.cpu, self.order, 2)  # Highlight a different slot.
        write32(self.cpu, self.manager + 8, 2)
        self.cpu.mem_write(self.profiles + 2 * 44, struct.pack("<II", 2, 9))
        write32(self.cpu, self.order + 4, 2)
        self.triangle()
        self.assertEqual(self.notes[-1][0], 9)
        self.assertNotEqual(len(self.rows), MAIN_ROWS)

    def test_helper_reports_each_stage(self):
        self.open()
        self.assertEqual([note[0] for note in self.notes], [5, 8])
        self.assertEqual(self.notes[0][1], 1)  # Main editor requested.

    def test_helper_never_calls_native_open_or_completion(self):
        self.open()
        self.select(14)
        self.assertEqual((self.opens, self.completions), ([], []))

    def test_unsupported_device_has_no_mapping_action(self):
        self.supported = False
        self.call("mapping_append", self.menu, self.widget, self.parent)
        self.assertEqual(self.rows, [])

    def test_guarded_image_and_displaced_code(self):
        self.assertEqual(self.call("gobt_mapping_supported", 0x06060010, BASE, 0x20C68, BASE), 1)
        self.assertEqual(self.call("gobt_mapping_supported", 0x06060110, BASE, 0x20C68, BASE), 1)
        write32(self.cpu, BASE + 0x10A80, 0)
        self.assertEqual(self.call("gobt_mapping_supported", 0x06060010, BASE, 0x20C68, BASE), 0)

    def test_old_helper_abi_is_rejected_without_changing_native_code(self):
        before = bytes(self.cpu.mem_read(BASE, 0x20C68))
        args = 0x0A606000
        for abi in (1, 2, 3):
            self.cpu.mem_write(args, struct.pack("<4I", abi, BASE, 0x20C68, self.service))
            self.assertEqual(self.call("module_start", 16, args), 0xFFFFFFFF)
        self.assertEqual(bytes(self.cpu.mem_read(BASE, 0x20C68)), before)

    def test_native_device_menu_preserves_actions_and_appends_mapping(self):
        self.native = True
        write32(self.cpu, self.menu_table + 0x15C, 0x0A700500)
        write32(self.cpu, self.menu_table + 0x1FC, 0x0A700600)
        args = 0x0A606000
        self.cpu.mem_write(args, struct.pack("<4I", UI_ABI, BASE, 0x20C68, self.service))
        self.assertEqual(self.call("module_start", 16, args), 0)
        # Execute the complete original device-context builder through the
        # generated append trampoline, with native UI methods mocked.
        invoke(self.cpu, BASE + 0x108DC, self.menu, self.parent)
        self.assertEqual([row[0] for row in self.rows], [
            "msgshare_connect", "msg_bt_disconnect", "msgshare_opt_del", "msgshare_opt_info", ENTRY])
        self.assertEqual(read32(self.cpu, self.symbols["device_widget"]), self.widget)
        self.assertEqual(self.binds, [b"page_bt_config_registered_option"])
        self.select(4)
        # The next Triangle runs Sony's builder; its rows were replaced before bind.
        self.assertEqual(len(self.rows), MAIN_ROWS)
        self.assertEqual(self.labels()[0], "Select / Create / -: Select")
        self.assertEqual(len(self.binds), 2)
        self.select(0)
        self.assertEqual(len(self.rows), BUTTON_CHOICES)

    def test_generic_option_menu_bypasses_hook_without_crashing(self):
        # Same class, other list: the builder skips every Add and jumps to the
        # second patched word. It must still load the page before binding.
        self.native = self.generic = True
        write32(self.cpu, self.menu_table + 0x15C, 0x0A700500)
        write32(self.cpu, self.menu_table + 0x1FC, 0x0A700600)
        args = 0x0A606000
        self.cpu.mem_write(args, struct.pack("<4I", UI_ABI, BASE, 0x20C68, self.service))
        self.assertEqual(self.call("module_start", 16, args), 0)
        self.assertNotEqual(read32(self.cpu, BASE + 0x10BB8), 0x08000000 | (((BASE + 0x10A84) >> 2) & 0x03FFFFFF))
        invoke(self.cpu, BASE + 0x108DC, self.menu, self.parent)
        self.assertEqual(self.rows, [])
        self.assertEqual(self.binds, [b"page_bt_config_option"])
        self.assertEqual(read32(self.cpu, self.symbols["device_widget"]), 0)  # Hook never entered.

    def test_relocated_firmware_capture(self):
        relocated = Path("pspgobt/bluetooth_plugin_module-050B8563.text.bin")
        if not relocated.exists():
            self.skipTest("second relocated Bluetooth capture not installed")
        self.cpu.mem_write(BASE + 0x100, relocated.read_bytes())
        self.assertEqual(self.call("gobt_mapping_supported", 0x06060010, BASE + 0x100, 0x20C68, BASE + 0x100), 1)

    def test_native_manager_constructor_and_array_allocation(self):
        allocations = []

        def allocation_mock(m, pc, _, __):
            a = m.reg_read(reg.UC_MIPS_REG_A0)
            result = None
            if pc == BASE + 0x1A0B0:  # Native order-vector constructor.
                m.mem_write(a, bytes(12))
                result = a
            elif pc == BASE + 0x1AB00:  # PAF new[] allocation.
                allocations.append(a)
                result = self.profiles - 4
            elif pc == BASE + 0x144F0:  # Each native 44-byte profile.
                m.mem_write(a, bytes(44))
                result = a
            elif pc == BASE + 0x12194:  # Refresh paired records: no radio call.
                result = 0
            if result is not None:
                m.reg_write(reg.UC_MIPS_REG_V0, result)
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        self.cpu.hook_add(UC_HOOK_CODE, allocation_mock)
        invoke(self.cpu, BASE + 0x12104, self.manager)
        self.assertEqual(read32(self.cpu, self.manager + 20), 9)
        self.assertEqual(read32(self.cpu, self.manager + 28), 0)
        invoke(self.cpu, BASE + 0x1238C, self.manager)
        self.assertEqual(allocations, [9 * 44 + 4])
        self.assertEqual(read32(self.cpu, self.profiles - 4), 9)
        self.assertEqual(read32(self.cpu, self.manager + 24), self.profiles)
        # Select the last paired slot. Element zero is reserved by Sony.
        write32(self.cpu, self.manager + 4, self.order)
        write32(self.cpu, self.manager + 8, 1)
        write32(self.cpu, self.order, 8)
        self.cpu.mem_write(self.profiles + 8 * 44, struct.pack("<II", 8, 9))
        self.call("mapping_append", self.menu, self.widget, self.parent)
        self.assertEqual([row[0] for row in self.rows], [ENTRY])
        self.assertEqual(read32(self.cpu, self.symbols["request"] + 4), 8)

    def test_unrelated_manager_state_does_not_hide_mapping(self):
        for state in (0, 1, 0x70000001):
            with self.subTest(state=state):
                self.rows.clear()
                write32(self.cpu, self.manager + 28, state)
                self.call("mapping_append", self.menu, self.widget, self.parent)
                self.assertEqual([row[0] for row in self.rows], [ENTRY])

    def test_invalid_array_cookie_and_reserved_or_out_of_range_slot_are_rejected(self):
        for index, cookie in ((0, 9), (9, 9), (1, 0), (1, 8)):
            with self.subTest(index=index, cookie=cookie):
                self.rows.clear()
                write32(self.cpu, self.order, index)
                write32(self.cpu, self.profiles - 4, cookie)
                self.call("mapping_append", self.menu, self.widget, self.parent)
                self.assertEqual(self.rows, [])


@unittest.skipUnless(CAPTURE.exists(), "local Bluetooth firmware capture not installed")
class NativeMenuLifecycle(unittest.TestCase):
    """Keep Sony's real Add, wide strings, vectors, clear and action dispatch.

    Only PAF memory/resources and final external UI/provider calls are mocked.
    The mapper ELF is relocated into real user RAM instead of executing at zero.
    """
    def setUp(self):
        self.case = MappingMenu("test_native_device_menu_preserves_actions_and_appends_mapping")
        self.case.UI_BASE = 0x0A640000
        self.case.setUp()
        self.cpu = self.case.cpu
        self.cpu.hook_del(self.case.mock_hook)
        self.cpu.mem_map(0x089A7000, 0x2000)
        self.cpu.mem_map(0x0A800000, 0x100000)
        self.next_heap = 0x0A800000
        self.live, self.callbacks, self.closes = {}, [], []
        self.resources = set()
        self.connected = False
        c = self.case
        self.cpu.mem_write(c.menu_table, bytes(self.cpu.mem_read(BASE + 0x1FD58, 0x280)))
        write32(self.cpu, c.menu_table + 0x48, c.show)
        write32(self.cpu, c.menu_table + 0x15C, 0x0A700500)
        write32(self.cpu, c.menu + 0x1C, 0x089A7200)
        write32(self.cpu, c.page_table + 0x16C, 0x0A700600)
        # Leaf UI effects outside this module: focus and close-begin view work.
        write32(self.cpu, c.menu_table + 0x19C, 0x0A700800)
        write32(self.cpu, c.menu_table + 0x1A0, 0x0A700900)
        c.emulate_close = False  # Sony's real completion, open and builder run.
        self.focus, self.notified, self.detached = [], [], []
        self.cpu.hook_add(UC_HOOK_CODE, self.mock)
        args = 0x0A606000
        self.cpu.mem_write(args, struct.pack("<4I", UI_ABI, BASE, 0x20C68, c.service))
        self.assertEqual(c.call("module_start", 16, args), 0)
        self.assertEqual(read32(self.cpu, BASE + VTABLE + CLOSE_SLOT), BASE + 0x91CC)

    def allocate(self, size):
        pointer = self.next_heap
        self.next_heap += (size + 15) & ~15
        self.assertLess(self.next_heap, 0x0A900000)
        self.live[pointer] = size
        self.cpu.mem_write(pointer, bytes(size))
        return pointer

    def mock(self, m, pc, _, __):
        a, b, count, _ = [m.reg_read(r) for r in ARGS]
        c = self.case
        result = None
        if pc in (BASE + 0x67D0, BASE + 0x15CB4, BASE + 0x16CC0, BASE + 0x158A0):
            return  # Execute these actual native implementations.
        if pc == BASE + 0x1AC10:
            result = self.allocate(a)
        elif pc == BASE + 0x1AC58:
            self.assertIn(a, self.live, f"Invalid native free: {a:08x}")
            self.live.pop(a)
            result = 0
        elif pc == BASE + 0x1A9C0:
            m.mem_write(a, bytes(m.mem_read(b, count)))
            result = a
        elif pc in (BASE + 0x1ACA8, BASE + 0x1AD20):
            width = 2 if pc == BASE + 0x1ACA8 else 1
            result = 0
            while bytes(m.mem_read(a + result * width, width)) != bytes(width):
                result += 1
        elif pc == BASE + 0x1AB68:
            value = string(m, b).decode().encode("utf-16le") + bytes(2)
            result = self.allocate(len(value))
            self.resources.add(result)
            m.mem_write(result, value)
        elif pc == BASE + 0x14818:
            result = int(self.connected)
        elif pc in (BASE + 0x14858, 0x0A700500):
            result = 0
        elif pc == 0x0A700600:
            self.assertEqual(a, c.page)
            self.closes.append(a)
            result = 0
        elif pc in (BASE + 0xFE00, BASE + 0x10088, BASE + 0xFD8C, BASE + 0x10578):
            self.assertEqual((a, count), (c.menu, c.widget))
            self.callbacks.append(pc)
            result = 0
        elif pc == c.show:
            # Attach: the builder attaches the page, open() then re-targets
            # the parent. Detach later clears menu+8.
            c.selections.append(read32(m, c.menu + 0x44))
            if b == c.page:
                write32(m, c.menu + 8, c.page)
            result = 0
        elif pc == 0x0A700800:
            self.focus.append(a)
            result = 0
        elif pc == 0x0A700900:
            result = 0
        elif pc == BASE + 0x3B3C:  # Page-side detach.
            self.detached.append((a, b))
            result = 0
        elif pc in (BASE + 0x3CEC, BASE + 0x3950):
            result = 0
        elif pc == BASE + 0x5874:
            # Native "menu closed" notification to the owner, delivered
            # synchronously from the stack event's constructor.
            self.assertEqual((a, read32(m, c.menu + 0x50)), (m.reg_read(reg.UC_MIPS_REG_SP), 0))
            self.notified.append(m.reg_read(reg.UC_MIPS_REG_A1))
            result = a
        elif BASE + 0x1A800 <= pc < BASE + 0x1AF00:
            self.fail(f"Unmocked PAF/provider import: {pc - BASE:05x}")
        else:
            c.mock(m, pc, _, __)
            return
        m.reg_write(reg.UC_MIPS_REG_V0, result)
        m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

    def rows(self):
        c = self.case
        pointer, count = read32(self.cpu, c.menu + 0x4C), read32(self.cpu, c.menu + 0x50)
        rows = []
        for i in range(count):
            text, length, _, callback, data = struct.unpack("<5I", self.cpu.mem_read(pointer + i * 20, 20))
            self.assertIn(text, self.live)
            rows.append((bytes(self.cpu.mem_read(text, length * 2)).decode("utf-16le"), callback, data))
        return rows

    def dispatch(self, index):
        event, source = 0x0A605000, 0x0A605100
        write32(self.cpu, event + 24, source)
        write32(self.cpu, source + 12, index)
        invoke(self.cpu, BASE + 0x7A6C, self.case.menu, event)

    def finish_close(self):
        """The page's asynchronous close: begin (+0x88), then complete (+0x8c)."""
        c = self.case
        invoke(self.cpu, read32(self.cpu, c.menu_table + 0x88), c.menu, 0x0A605000)
        self.assertEqual(read32(self.cpu, c.menu + 0x2C) & 4, 4)  # Closing.
        self.assertEqual(read32(self.cpu, c.menu + 8), 0)         # Detached.
        invoke(self.cpu, read32(self.cpu, c.menu_table + CLOSE_SLOT), c.menu, 0x0A605000)

    def select(self, index):
        c = self.case
        before, binds = self.rows(), len(c.binds)
        self.dispatch(index)
        # Nothing is rebuilt or re-bound while the dispatcher's close runs.
        self.assertEqual((self.rows(), len(c.binds)), (before, binds))
        self.finish_close()

    def triangle(self):
        c = self.case
        invoke(self.cpu, BASE + 0x9DD4, c.menu, c.parent, 0x0A606100, 0)

    def test_native_items_keep_their_callbacks_and_dispatch_after_page_close(self):
        lookup = bytes(self.cpu.mem_read(BASE + 0x11E4, 8))
        invoke(self.cpu, BASE + 0x108DC, self.case.menu, self.case.parent)
        rows = self.rows()
        self.assertEqual([r[0] for r in rows], ["msgshare_connect", "msg_bt_disconnect", "msgshare_opt_del", "msgshare_opt_info", ENTRY])
        self.assertEqual([r[1] for r in rows[:4]], [BASE + 0xFE00, 0, BASE + 0xFD8C, BASE + 0x10578])
        for index in range(4):
            self.dispatch(index)
        self.assertEqual(self.callbacks, [BASE + 0xFE00, BASE + 0xFD8C, BASE + 0x10578])
        self.assertEqual(len(self.closes), 3)
        self.assertEqual(bytes(self.cpu.mem_read(BASE + 0x11E4, 8)), lookup)

    def test_native_vector_string_lifetimes_across_edit_save_and_cancel(self):
        c = self.case
        self.cpu.mem_write(0x0A606100, b"\0")
        self.triangle()
        self.assertEqual(self.rows()[-1][0], ENTRY)
        self.assertEqual(self.focus, [c.menu])
        self.select(4)
        # The native close completes and nothing reopens on its own.
        self.assertEqual((self.rows(), len(self.focus), len(self.notified)), ([], 1, 1))
        self.triangle()
        self.assertEqual(len(self.rows()), MAIN_ROWS)
        self.assertEqual(self.focus, [c.menu, c.menu])
        self.assertEqual(read32(self.cpu, c.menu + 8), c.page)
        self.select(14); self.triangle()
        self.assertEqual(len(self.rows()), BUTTON_CHOICES)
        self.assertEqual(c.selections[-2], 14)
        self.select(13); self.triangle()
        self.assertIn("Cross / B: Circle", [r[0] for r in self.rows()])
        self.select(20)
        self.assertEqual(len(c.saves), 1)
        self.assertEqual(c.saves[0][30], 13)
        self.assertEqual((self.rows(), len(self.focus)), ([], 4))
        self.triangle()
        self.assertEqual(self.rows()[-1][0], ENTRY)
        # Cancel ends the same way.
        self.select(4); self.triangle()
        self.select(22)
        self.assertEqual(self.rows(), [])
        # All native strings and vectors were freed by Sony's own clear.
        self.assertEqual(set(self.live), self.resources)

    def test_back_button_closes_editor_and_next_triangle_is_native(self):
        self.cpu.mem_write(0x0A606100, b"\0")
        self.triangle()
        self.select(4)
        self.triangle()
        self.assertEqual(len(self.rows()), MAIN_ROWS)
        # Circle/back: the page closes without any row being selected.
        self.finish_close()
        self.assertEqual(self.rows(), [])
        self.triangle()
        self.assertEqual([r[0] for r in self.rows()], ["msgshare_connect", "msg_bt_disconnect", "msgshare_opt_del", "msgshare_opt_info", ENTRY])

    def test_native_actions_never_reopen_the_menu(self):
        self.cpu.mem_write(0x0A606100, b"\0")
        self.triangle()
        for index in (0, 2, 3):
            focus = len(self.focus)
            self.select(index)
            self.assertEqual((self.rows(), len(self.focus)), ([], focus))
            self.triangle()
        self.assertEqual(self.callbacks, [BASE + 0xFE00, BASE + 0xFD8C, BASE + 0x10578])

    def test_connected_controller_keeps_native_disconnect_and_information_actions(self):
        self.connected = True
        invoke(self.cpu, BASE + 0x108DC, self.case.menu, self.case.parent)
        self.assertEqual([r[1] for r in self.rows()[:4]], [0, BASE + 0x10088, 0, BASE + 0x10578])
        for index in range(4):
            self.dispatch(index)
        self.assertEqual(self.callbacks, [BASE + 0x10088, BASE + 0x10578])
        self.assertEqual(len(self.closes), 2)


if __name__ == "__main__":
    unittest.main()
