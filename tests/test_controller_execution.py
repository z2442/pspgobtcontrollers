"""Run the compiled sink through captured Sony sampling and application reads."""
from pathlib import Path
import struct
import unittest
import zlib

from unicorn import UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import machine, SYMBOLS


class ControllerSampler(unittest.TestCase):
    def exercise(self, game, foreign=False, changed_signature=False):
        uid, base = ("018CC807", 0x880E6700) if game else ("018B8117", 0x880E6200)
        path = Path(f"pspgobt/sceController_Service-{uid}.text.bin")
        if not path.exists():
            self.skipTest("local controller firmware capture not present")
        cpu, clock = machine(), [1000]
        cpu.mem_map(0x08000000, 0x100000)
        physical = lambda p: p & 0x1FFFFFFF if p >= 0x80000000 else p
        cpu.mem_write(physical(base), path.read_bytes())
        state = base + 0x2890

        def put(pointer, value):
            cpu.mem_write(physical(pointer), struct.pack("<I", value))

        # Supply native-allocated ring buffers and an existing port. Sony's
        # real registration replaces its handler, without testing allocation.
        for offset, pointer in ((0xF8, 0x880F0000), (0x120, 0x880F1000),
                                (0xFC, 0x880F2000), (0x100, 0x880F3000),
                                (0x124, 0x880F4000), (0x128, 0x880F5000)):
            put(state + offset, pointer)
        for offset in (0x298, 0x29C):
            put(state + offset, 0x3FFFF)
        cpu.mem_write(physical(state + 12), b"\x01\x01")
        put(state + 0x304, 0x0A600200)

        def observe(m, address, _, __):
            # Unicorn lacks Allegrex MIN/MAX. A MIN in a REGIMM delay slot
            # requires emulating the branch and slot together, before entry.
            instruction = struct.unpack("<I", m.mem_read(physical(address), 4))[0]
            delay = struct.unpack("<I", m.mem_read(physical(address + 4), 4))[0]
            signed = lambda value: value - 0x100000000 if value & 0x80000000 else value
            if instruction >> 26 == 1 and delay & 0xFC0007FF in (0x2C, 0x2D):
                rs, rt = (instruction >> 21) & 31, (instruction >> 16) & 31
                self.assertIn(rt, (0, 1))
                value = signed(m.reg_read(reg.UC_MIPS_REG_0 + rs))
                take = value < 0 if rt == 0 else value >= 0
                drs, drt, drd = (delay >> 21) & 31, (delay >> 16) & 31, (delay >> 11) & 31
                operation = max if delay & 0x3F == 0x2C else min
                m.reg_write(reg.UC_MIPS_REG_0 + drd, operation(
                    signed(m.reg_read(reg.UC_MIPS_REG_0 + drs)),
                    signed(m.reg_read(reg.UC_MIPS_REG_0 + drt))) & 0xFFFFFFFF)
                offset = instruction & 0xFFFF
                if offset & 0x8000:
                    offset -= 0x10000
                m.reg_write(reg.UC_MIPS_REG_PC, address + 4 + offset * 4 if take else address + 8)
                return
            a, b, c = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2)]
            returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                       SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                       SYMBOLS["sceKernelGetSystemTimeWide"]: clock[0],
                       SYMBOLS["sceKernelRegisterSysEventHandler"]: 0,
                       SYMBOLS["sceKernelUnregisterSysEventHandler"]: 0}
            if base + 0x21AC <= address < base + 0x22C0:
                returns[address] = clock[0] if address == base + 0x223C else 0
            if address == SYMBOLS["sctrlHENFindFunction"]:
                self.assertIn(c, (0xE467BEC8, 0x6C86AF22))
                returns[address] = base + (0x11F0 if c == 0xE467BEC8 else 0x1AA8)
            if address == SYMBOLS.get("memset"):
                m.mem_write(physical(a), bytes([b & 255]) * c)
                returns[address] = a
            if address == SYMBOLS.get("memcpy"):
                m.mem_write(physical(a), bytes(m.mem_read(physical(b), c)))
                returns[address] = a
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_V1, 0)
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        def invoke(address, args, k1=0):
            cpu.reg_write(reg.UC_MIPS_REG_SP, 0x0A60F000)
            cpu.reg_write(reg.UC_MIPS_REG_RA, 0x0A700F00)
            cpu.reg_write(reg.UC_MIPS_REG_K1, k1)
            for r, value in zip((reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                                 reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3), args):
                cpu.reg_write(r, value)
            cpu.emu_start(address, 0x0A700F00, count=200000)
            self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_PC), 0x0A700F00)
            self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)
            return cpu.reg_read(reg.UC_MIPS_REG_V0)

        cpu.hook_add(UC_HOOK_CODE, observe)
        self.assertEqual(invoke(SYMBOLS["gobt_sink_start"], [0]), 0)
        self.assertEqual(struct.unpack("<I", cpu.mem_read(physical(state + 0x300), 4))[0], 1)
        report = bytearray(78)
        report[0], report[2:8], report[9:12] = 0x31, bytes([11, 22, 33, 44, 55, 66]), bytes([0x20, 1, 1])
        struct.pack_into("<I", report, 74, zlib.crc32(b"\xA1" + report[:74]))
        cpu.mem_write(0x0A600300, bytes(report))
        self.assertEqual(invoke(SYMBOLS["pspgobtSubmitReport"], [0x0A600300, 78]), 0)
        self.assertEqual(invoke(base + 0x968, [0, 128, 128]), 0)
        for negative in (0, 1):
            self.assertEqual(invoke(base + 0x1E70, [0x0A600400, 1, 0, negative], 0x100000), 1)
            frame = bytes(cpu.mem_read(0x0A600400, 16))
            buttons = struct.unpack_from("<I", frame, 4)[0]
            # Sony maps the DS3 L1 state to the PSP L trigger in standard reads.
            self.assertEqual(buttons, 0xFFFEBEEF if negative else 0x14110)
            self.assertEqual(frame[8:10], bytes([11, 22]))
        clock[0] += 250000
        self.assertEqual(invoke(base + 0x968, [0, 128, 128]), 0)
        self.assertEqual(invoke(base + 0x1E70, [0x0A600400, 1, 0, 0], 0x100000), 1)
        frame = bytes(cpu.mem_read(0x0A600400, 16))
        self.assertEqual(struct.unpack_from("<I", frame, 4)[0], 0)
        self.assertEqual(frame[8:10], bytes([128, 128]))
        # Detach only this instance's port while preserving port-2 passthrough.
        put(state + 0x300, 3)
        if foreign:
            put(state + 0x304, 0x0A600200)
            put(state + 0x30C, 0x0A600220)
        if changed_signature:
            put(base + 0x11F0, 0)
        previous_handler = struct.unpack("<I", cpu.mem_read(physical(state + 0x304), 4))[0]
        invoke(SYMBOLS["gobt_sink_shutdown"], [])
        current_handler = struct.unpack("<I", cpu.mem_read(physical(state + 0x304), 4))[0]
        self.assertEqual(current_handler, previous_handler if foreign or changed_signature else 0)
        self.assertEqual(struct.unpack("<I", cpu.mem_read(physical(state + 0x300), 4))[0],
                         3 if foreign or changed_signature else 2)
        self.assertEqual(invoke(SYMBOLS["gobt_sink_start"], [0]), 0xFFFFFFFF)
        self.assertEqual(invoke(SYMBOLS["pspgobtSubmitReport"], [0x0A600300, 78]), 0xFFFFFFFF)
        invoke(SYMBOLS["gobt_sink_shutdown"], [])  # idempotent final reboot phase

    def test_xmb_driver_passes_input_to_regular_user_reads(self):
        self.exercise(False)

    def test_game_driver_passes_input_to_regular_user_reads(self):
        self.exercise(True)

    def test_shutdown_preserves_a_replacement_handler(self):
        self.exercise(True, foreign=True)

    def test_shutdown_refuses_changed_controller_accessor(self):
        self.exercise(False, changed_signature=True)


if __name__ == "__main__":
    unittest.main()
