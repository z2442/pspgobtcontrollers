"""Execute bridge installation, late providers, and controller config I/O."""
from pathlib import Path
import struct
import unittest
import zlib

from unicorn import UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import machine, SYMBOLS, word


class Installation:
    def __init__(self, case, game=False):
        self.case, self.cpu = case, machine()
        self.cpu.mem_map(0x08100000, 0x200000)
        self.bsbase, self.cdcbase = (0x8820B200, 0x881FA000) if game else (0x88238900, 0x88226700)
        bsuid, cdcuid = ("03F23B65", "03F3DF17") if game else ("04540F41", "03D9D063")
        self.module, self.cdc = 0x0A600000, 0x0A600200
        for pointer, name, base, uid in ((self.module, "sceBSMan", self.bsbase, bsuid),
                                         (self.cdc, "sceUsbBSMCDC_Driver", self.cdcbase, cdcuid)):
            path = Path(f"pspgobt/{name}-{uid}.text.bin")
            if not path.exists():
                case.skipTest("local firmware captures not present")
            data = path.read_bytes()
            self.cpu.mem_write(self.physical(base), data)
            self.cpu.mem_write(pointer + 8, name.encode() + b"\0")
            self.cpu.mem_write(pointer + 44, struct.pack("<I", 7))
            self.cpu.mem_write(pointer + 108, struct.pack("<II", base, len(data)))
        self.bs_available = self.cdc_available = True
        self.power, self.created, self.started = [], [], []
        self.clock, self.cache_data, self.writes = 1000, None, []
        self.write_result = 32
        self.wait_result, self.waits, self.deleted = 0, [], []
        self.sema_result, self.create_result, self.start_result = 10, 8, 0
        self.semas, self.sema_deleted, self.signals, self.input_waits = [], [], [], []
        self.unregistered, self.priorities = [], []
        self.cpu.hook_add(UC_HOOK_CODE, self.observe)

    @staticmethod
    def physical(pointer):
        return pointer & 0x1FFFFFFF if pointer >= 0x80000000 else pointer

    def read_word(self, name):
        return struct.unpack("<I", self.cpu.mem_read(SYMBOLS[name], 4))[0]

    def cstring(self, m, pointer):
        return bytes(m.mem_read(self.physical(pointer), 96)).split(b"\0", 1)[0]

    def observe(self, m, address, _, __):
        a, b, c = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2)]
        returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                   SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                   SYMBOLS["sceKernelDcacheWritebackInvalidateAll"]: 0,
                   SYMBOLS["sceKernelIcacheInvalidateAll"]: 0,
                   SYMBOLS["sceKernelGetSystemTimeWide"]: self.clock,
                   SYMBOLS["sceIoClose"]: 0, SYMBOLS["gobt_log"]: 0}
        if address == SYMBOLS["sceIoOpen"]:
            path = self.cstring(m, a)
            if path.endswith(b"/controllers.cfg"):
                returns[address] = 9 if b != 1 or self.cache_data is not None else 0xFFFFFFFF
            else:
                self.case.assertTrue(path.endswith(b"/input-test"))
                returns[address] = 0xFFFFFFFF
        if address == SYMBOLS["sceIoRead"]:
            self.case.assertEqual((a, c), (9, 33))
            m.mem_write(self.physical(b), self.cache_data)
            returns[address] = len(self.cache_data)
        if address == SYMBOLS["sceIoWrite"]:
            self.case.assertEqual((a, c), (9, 32))
            self.writes.append(bytes(m.mem_read(self.physical(b), c)))
            returns[address] = self.write_result
        if address == SYMBOLS["sceKernelFindModuleByName"]:
            name = self.cstring(m, a)
            returns[address] = (self.module if self.bs_available else 0) if name == b"sceBSMan" else (self.cdc if self.cdc_available else 0)
        if address == SYMBOLS["sctrlHENFindFunction"]:
            returns[address] = {0xA5E108BB: self.cdcbase + 0x1514,
                                0xF8C6AFBD: self.cdcbase + 0x1640,
                                0xCBD232D1: self.bsbase + 0x4480}[c] if self.cdc_available else 0
        if address == SYMBOLS["sceKernelRegisterSysEventHandler"]:
            self.power.append(a)
            returns[address] = 0
        if address == SYMBOLS["sceKernelUnregisterSysEventHandler"]:
            self.unregistered.append(a)
            returns[address] = 0
        if address == SYMBOLS["sceKernelCreateSema"]:
            self.case.assertEqual(self.cstring(m, a), b"PSPGoBTInputReady")
            self.case.assertEqual((b, c, m.reg_read(reg.UC_MIPS_REG_A3),
                                   m.reg_read(reg.UC_MIPS_REG_T0)), (0, 0, 1, 0))
            self.semas.append(a)
            returns[address] = self.sema_result
        if address == SYMBOLS["sceKernelDeleteSema"]:
            self.sema_deleted.append(a)
            returns[address] = 0
        if address == SYMBOLS["sceKernelSignalSema"]:
            self.case.assertEqual((a, b, m.reg_read(reg.UC_MIPS_REG_K1)), (10, 1, 0))
            self.signals.append(a)
            returns[address] = 0
        if address == SYMBOLS["sceKernelWaitSema"]:
            self.case.assertEqual((a, b, struct.unpack("<I", m.mem_read(c, 4))[0]), (10, 1, 50000))
            self.input_waits.append(a)
            m.emu_stop()
        if address == SYMBOLS["sceKernelCreateThread"]:
            self.created.append(a)
            self.priorities.append(c)
            returns[address] = self.create_result
        if address == SYMBOLS["sceKernelStartThread"]:
            self.started.append(a)
            returns[address] = self.start_result
        if address == SYMBOLS["sceKernelWaitThreadEnd"]:
            self.case.assertEqual(a, 8)
            self.case.assertEqual(struct.unpack("<I", m.mem_read(b, 4))[0], 250000)
            self.waits.append(a)
            returns[address] = self.wait_result
        if address == SYMBOLS["sceKernelDeleteThread"]:
            self.deleted.append(a)
            returns[address] = 0
        if address == SYMBOLS.get("strcmp"):
            returns[address] = int(self.cstring(m, a) != self.cstring(m, b))
        if address == SYMBOLS.get("memcmp"):
            returns[address] = int(bytes(m.mem_read(self.physical(a), c)) != bytes(m.mem_read(self.physical(b), c)))
        if address == SYMBOLS.get("memcpy"):
            data = bytes(m.mem_read(self.physical(b), c))
            # Model relocation of the nine local target pointers into KSEG0.
            # The rest of this test executes the ELF at address zero.
            if c == 36 and struct.unpack("<I", data[:4])[0] == SYMBOLS["submit_first"]:
                data = struct.pack("<9I", *(value + 0x88050000 for value in struct.unpack("<9I", data)))
            m.mem_write(self.physical(a), data)
            returns[address] = a
        if address == SYMBOLS.get("memset"):
            m.mem_write(self.physical(a), bytes([b & 255]) * c)
            returns[address] = a
        if address == SYMBOLS["sceKernelDelayThread"]:
            m.emu_stop()
        if address in returns:
            m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
            m.reg_write(reg.UC_MIPS_REG_V1, 0)
            m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

    def call(self, name, *args):
        self.cpu.reg_write(reg.UC_MIPS_REG_SP, 0x0A60F000)
        self.cpu.reg_write(reg.UC_MIPS_REG_RA, 0x0A700F00)
        for r, value in zip((reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1), args):
            self.cpu.reg_write(r, value)
        self.cpu.emu_start(SYMBOLS[name], 0x0A700F00, count=300000)
        return self.cpu.reg_read(reg.UC_MIPS_REG_V0)


class Installer(unittest.TestCase):
    def test_full_installer_accepts_xmb_and_game_firmware(self):
        for game in (False, True):
            with self.subTest(game=game):
                s = Installation(self, game)
                self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
                self.assertEqual((s.read_word("installed"), s.read_word("worker_started")), (1, 1))
                self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
                self.assertEqual((len(s.power), len(s.created), len(s.started)), (1, 1, 1))

    def test_retry_worker_survives_initial_absence_and_provider_lookup_failure(self):
        s = Installation(self, True)
        s.bs_available = s.cdc_available = False
        self.assertEqual(s.call("gobt_start_input_bridge", 0x06060010), 0)
        s.call("worker", 0, 0)
        self.assertEqual(s.read_word("installed"), 0)
        s.bs_available = True
        s.call("worker", 0, 0)
        self.assertEqual(s.read_word("installed"), 0)
        s.cdc_available = True
        s.call("worker", 0, 0)
        self.assertEqual(s.read_word("installed"), 1)
        self.assertEqual((len(s.power), len(s.created), len(s.started)), (1, 1, 1))

    def test_cache_file_loads_before_worker_start_and_short_writes_remain_dirty(self):
        s = Installation(self)
        data = bytearray(b"GBTC\x01\x01\0\0" + bytes([1, 2, 3, 4, 5, 6]) + bytes(14))
        data += struct.pack("<I", zlib.crc32(b"\0" + data))
        s.cache_data = bytes(data)
        self.assertEqual(s.call("gobt_start_input_bridge", 0x06060010), 0)
        self.assertEqual(s.read_word("target_cache"), 1)
        s.call("worker", 0, 0)  # Bind providers, then exercise worker persistence.
        word(s.cpu, "cache_dirty", 1)
        word(s.cpu, "cache_revision", 1)
        s.write_result = 31
        s.call("worker", 0, 0)
        self.assertEqual(s.read_word("cache_dirty"), 1)
        self.assertEqual(s.writes, [bytes(data)])
        s.write_result, s.clock = 32, 1001000
        s.call("worker", 0, 0)
        self.assertEqual(s.read_word("cache_dirty"), 0)
        self.assertEqual(s.writes[-1], bytes(data))


if __name__ == "__main__":
    unittest.main()
