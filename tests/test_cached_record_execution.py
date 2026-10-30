"""Execute the cached-record query ABI without remote refresh/discovery."""
import struct
import unittest

from unicorn import UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import machine, SYMBOLS, word


class CachedRecord(unittest.TestCase):
    def exercise(self, status=0, result=0, slot=1, empty=False, device=1, size=80):
        cpu = machine()
        command, destination = 0x0A700100, 0x0A600010
        word(cpu, "record_command", command)
        before = b"\xCC" * 16 + struct.pack("<I", size) + b"\xCC" * 92
        cpu.mem_write(destination - 16, before)
        raw = bytearray(80)
        struct.pack_into("<H", raw, 0, slot)
        if not empty:
            name = "DualSense Wireless Controller".encode("utf-16le")
            raw[2:2 + len(name)] = name
            struct.pack_into("<H", raw, 68, 0x402)
        raw[72:] = b"\xAA" * 8  # unused record tail must not escape classification
        calls = []

        def observe(m, address, _, __):
            args = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                                           reg.UC_MIPS_REG_A2)]
            returns = {SYMBOLS["gobt_log"]: 0}
            if address == command:
                cmd, length, payload = struct.unpack("<HHI", m.mem_read(args[0], 8))
                expected, capacity, _, reserved, output = struct.unpack("<HHHHI", m.mem_read(args[1], 12))
                self.assertEqual((cmd, length, expected, capacity, reserved, args[2]),
                                 (0x63, 2, 0x163, 80, 0, 0))
                self.assertEqual(struct.unpack("<H", m.mem_read(payload, 2))[0], device)
                self.assertEqual(bytes(m.mem_read(output, 80)), bytes(80))
                m.mem_write(output, bytes(raw))
                m.mem_write(args[1] + 4, struct.pack("<H", status))
                calls.append(cmd)
                returns[address] = result
            if address == SYMBOLS.get("memset"):
                m.mem_write(args[0], bytes([args[1] & 255]) * args[2])
                returns[address] = args[0]
            if address == SYMBOLS.get("memcpy"):
                m.mem_write(args[0], bytes(m.mem_read(args[1], args[2])))
                returns[address] = args[0]
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.reg_write(reg.UC_MIPS_REG_A0, device)
        cpu.reg_write(reg.UC_MIPS_REG_A1, destination)
        cpu.emu_start(SYMBOLS["cached_info"], 0x0A700F00, count=20000)
        invalid = device < 1 or device > 8 or size != 80
        failure = invalid or status or result & 0x80000000 or (not empty and slot != device)
        self.assertEqual(calls, [] if invalid else [0x63])
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0),
                         result if result & 0x80000000 and not invalid else 0xFFFFFFFF if failure else 0)
        actual = bytes(cpu.mem_read(destination - 16, 112))
        if failure:
            self.assertEqual(actual, before)
        else:
            normalized = bytearray(80)
            struct.pack_into("<I", normalized, 0, 80)
            if not empty:
                normalized[4:72] = raw[:68]
                normalized[72:74] = raw[68:70]
            self.assertEqual(actual, b"\xCC" * 16 + normalized + b"\xCC" * 16)

    def test_cached_name_and_capabilities_without_refresh(self):
        self.exercise()

    def test_empty_record_stays_empty(self):
        self.exercise(empty=True, slot=0)

    def test_transport_failure_is_preserved(self):
        self.exercise(result=0x802F0203)

    def test_device_status_failure_is_rejected(self):
        self.exercise(status=1)

    def test_wrong_slot_cannot_select_controller(self):
        self.exercise(slot=2)

    def test_invalid_slot_and_buffer_size_never_issue_commands(self):
        for device, size in ((0, 80), (9, 80), (1, 79)):
            with self.subTest(device=device, size=size):
                self.exercise(device=device, size=size)


if __name__ == "__main__":
    unittest.main()
