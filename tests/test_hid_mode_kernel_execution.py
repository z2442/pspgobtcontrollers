"""Execute kernel selection, cache guards and installation of the HID mode."""
from pathlib import Path
import struct
import unittest
from unicorn import UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import machine, SYMBOLS, word
import test_reconnect_execution as reconnect


class KernelMode(unittest.TestCase):
    def selection(self, record=True, known=False, matches=True):
        cpu = machine()
        profile, info = 0x0A600000, 0x0A600100
        cpu.mem_write(profile, struct.pack("<II", 3, 8) + bytes(36))
        data = bytearray(80)
        struct.pack_into("<I", data, 0, 80)
        name = ("DualSense Wireless Controller" if matches else "Audio Device").encode("utf-16le")
        data[6:6 + len(name)] = name
        struct.pack_into("<H", data, 72, 0x402)
        cpu.mem_write(info, bytes(data))
        calls = []
        cpu.reg_write(reg.UC_MIPS_REG_K1, 0x12340000)

        def observe(m, address, _, __):
            if address in (SYMBOLS["sceKernelCpuSuspendIntr"], SYMBOLS["sceKernelCpuResumeIntr"]):
                m.reg_write(reg.UC_MIPS_REG_V0, 0)
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))
            if address == SYMBOLS["gobt_input_bridge_known"]:
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_K1), 0)
                calls.append(m.reg_read(reg.UC_MIPS_REG_A0))
                m.reg_write(reg.UC_MIPS_REG_V0, int(known))
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))
        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.reg_write(reg.UC_MIPS_REG_A0, profile)
        cpu.reg_write(reg.UC_MIPS_REG_A1, info if record else 0)
        cpu.reg_write(reg.UC_MIPS_REG_A2, 3)
        cpu.emu_start(SYMBOLS["pspgobtRouteStoredHid"], 0x0A700F00, count=10000)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_K1), 0x12340000)
        self.assertEqual(bytes(cpu.mem_read(info, 80)), bytes(data))
        selected = matches if record else known
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), int(selected))
        self.assertEqual(struct.unpack("<II", cpu.mem_read(profile, 8)), (3, 9 if selected else 8))
        self.assertEqual(calls, [] if record else [3])

    def test_saved_decision_preserves_k1_and_metadata_and_requires_positive_selection(self):
        for record, known, matches in ((True, False, True), (True, True, False),
                                        (False, True, True), (False, False, True)):
            self.selection(record, known, matches)

    def test_cache_fallback_requires_current_paired_address_even_if_old_prepared_bit_is_set(self):
        s = reconnect.Session(self)
        reconnect.Reconnect.paired_target(self, s)
        self.assertEqual(s.invoke("gobt_input_bridge_known", 1), 1)
        word(s.cpu, "prepared_devices", 2)
        s.cpu.mem_write(s.physical(0x88205000 + 0x24 + 6), b"\x07" * 6)
        self.assertEqual(s.invoke("gobt_input_bridge_known", 1), 0)
        self.assertEqual(s.invoke("gobt_input_bridge_known", 9), 0)

    def test_installer_patches_all_mode_paths_together_and_rejects_changed_signature(self):
        for uid, base in (("04FE4C4D", 0x0A572000), ("050B8563", 0x0A572100)):
            path = Path(f"pspgobt/bluetooth_plugin_module-{uid}.text.bin")
            if not path.exists():
                self.skipTest("requires local Sony capture")
            for damaged in (False, True):
                cpu, calls = machine(), []
                cpu.mem_map(0x0A570000, 0x30000)
                cpu.mem_write(base, path.read_bytes())
                if damaged:
                    cpu.mem_write(base + 0x151DC, bytes(4))
                module, allocation = 0x0A600000, 0x0A600800
                cpu.mem_write(module + 8, b"bluetooth_plugin_module\0")
                cpu.mem_write(module + 108, struct.pack("<II", base, len(path.read_bytes())))
                def observe(m, address, _, __):
                    a, b, c = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2)]
                    returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                               SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                               SYMBOLS["sceKernelDcacheWritebackInvalidateAll"]: 0,
                               SYMBOLS["sceKernelIcacheInvalidateAll"]: 0,
                               SYMBOLS["sceKernelGetBlockHeadAddr"]: allocation,
                               SYMBOLS["gobt_log"]: 0}
                    if address == SYMBOLS["sctrlHENMakeSyscallStub"]:
                        calls.append("stub")
                        returns[address] = 0x0A700100 if a == SYMBOLS["pspgobtRouteHid"] else 0x0A700120
                    if address == SYMBOLS["sceKernelAllocPartitionMemory"]:
                        calls.append("allocate")
                        returns[address] = 10
                    if address == SYMBOLS.get("strcmp"):
                        left, right = [bytes(m.mem_read(p, 32)).split(b"\0", 1)[0] for p in (a, b)]
                        returns[address] = int(left != right)
                    if address == SYMBOLS.get("memcmp"):
                        returns[address] = int(bytes(m.mem_read(a, c)) != bytes(m.mem_read(b, c)))
                    if address == SYMBOLS.get("memcpy"):
                        m.mem_write(a, bytes(m.mem_read(b, c)))
                        returns[address] = a
                    if address in returns:
                        m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                        m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))
                cpu.hook_add(UC_HOOK_CODE, observe)
                cpu.reg_write(reg.UC_MIPS_REG_A0, module)
                cpu.reg_write(reg.UC_MIPS_REG_A1, 0x06060010)
                cpu.emu_start(SYMBOLS["gobt_install_hid_route"], 0x0A700F00, count=100000)
                self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), 0xFFFFFFFE if damaged else 0)
                if damaged:
                    self.assertEqual(calls, [])
                    self.assertEqual(bytes(cpu.mem_read(base + 0x146C4, 8)), path.read_bytes()[0x146C4:0x146CC])
                else:
                    self.assertEqual(calls, ["stub", "stub", "allocate"])
                    for offset, target, opcode in ((0x1512C, allocation, 3),
                        (0x151DC, allocation + 21 * 4, 3),
                        (0x146C4, allocation + (21 + 29) * 4, 2),
                        (0x14D34, allocation + (21 + 29 + 12) * 4, 2)):
                        instruction = struct.unpack("<I", cpu.mem_read(base + offset, 4))[0]
                        self.assertEqual(instruction, opcode << 26 | target >> 2)
                    for offset in (0x146C8, 0x14D38):
                        self.assertEqual(bytes(cpu.mem_read(base + offset, 4)), bytes(4))


if __name__ == "__main__":
    unittest.main()
