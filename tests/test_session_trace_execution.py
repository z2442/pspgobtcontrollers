"""Execute the optional bounded trace producer/writer; no Bluetooth mocks."""
import struct
import unittest
from unicorn import UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import machine, SYMBOLS, word


class Trace(unittest.TestCase):
    def setUp(self):
        if "trace_ring" not in SYMBOLS:
            self.skipTest("requires PSPGOBT_SESSION_TRACE=1")
        self.cpu = machine()
        self.opens, self.writes, self.closes = [], [], []
        self.file_length, self.write_error, self.open_error = 0, False, False
        self.cpu.hook_add(UC_HOOK_CODE, self.observe)

    def value(self, name):
        return struct.unpack("<I", self.cpu.mem_read(SYMBOLS[name], 4))[0]

    def observe(self, m, address, _, __):
        a, b, c = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2)]
        returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                   SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                   SYMBOLS["sceKernelGetSystemTimeWide"]: 12345,
                   SYMBOLS["sceKernelCreateThread"]: 20,
                   SYMBOLS["sceKernelStartThread"]: 0,
                   SYMBOLS["sceKernelDeleteThread"]: 0}
        if address == SYMBOLS["sceIoOpen"]:
            self.assertTrue(bytes(m.mem_read(a, 48)).startswith(b"ef0:/seplugins/pspgobt/session.log\0"))
            self.opens.append(b)
            returns[address] = 0xFFFFFFFF if self.open_error else 9
        if address == SYMBOLS["sceIoLseek"]:
            returns[address] = self.file_length
        if address == SYMBOLS["sceIoWrite"]:
            self.assertEqual((a, c), (9, 63))
            self.writes.append(bytes(m.mem_read(b, c)))
            returns[address] = 0xFFFFFFFF if self.write_error else c
        if address == SYMBOLS["sceIoClose"]:
            self.closes.append(a)
            returns[address] = 0
        if address == SYMBOLS.get("memcpy"):
            m.mem_write(a, bytes(m.mem_read(b, c)))
            returns[address] = a
        if address == SYMBOLS["sceKernelWaitThreadEnd"]:
            self.assertEqual(a, 20)
            self.assertEqual(struct.unpack("<I", m.mem_read(b, 4))[0], 100000)
            returns[address] = 0x800201A8  # timeout; must never terminate thread
        if address == SYMBOLS["sceKernelDelayThread"]:
            m.emu_stop()
        if address in returns:
            m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
            m.reg_write(reg.UC_MIPS_REG_V1, 0)
            m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

    def call(self, name, *args):
        self.cpu.reg_write(reg.UC_MIPS_REG_SP, 0x0A60F000)
        self.cpu.reg_write(reg.UC_MIPS_REG_RA, 0x0A700F00)
        for r, v in zip((reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2,
                         reg.UC_MIPS_REG_A3, reg.UC_MIPS_REG_T0), args):
            self.cpu.reg_write(r, v)
        self.cpu.emu_start(SYMBOLS[name], 0x0A700F00, count=500000)

    def test_callback_is_ram_only_and_preserves_k1(self):
        self.call("gobt_session_trace_start", 0x06060010)
        self.cpu.reg_write(reg.UC_MIPS_REG_K1, 0x12340000)
        self.call("gobt_session_trace", 6, 1, 7, 8, 9)
        self.assertEqual(self.cpu.reg_read(reg.UC_MIPS_REG_K1), 0x12340000)
        self.assertEqual((self.opens, self.writes), ([], []))
        self.assertEqual(self.value("trace_count"), 2)
        self.call("trace_writer", 0, 0)
        self.assertEqual(len(self.writes), 2)
        self.assertEqual(self.writes[1], b"00003039 00000002 00000006 00000001 00000007 00000008 00000009\n")

    def test_ring_overflow_is_bounded_and_sequence_exposes_loss(self):
        self.call("gobt_session_trace_start", 0x06060010)
        for i in range(150):
            self.call("gobt_session_trace", 2, 1, i, 0, 0)
        self.assertEqual(self.value("trace_count"), 128)
        self.call("trace_writer", 0, 0)
        self.assertEqual(len(self.writes), 128)
        self.call("gobt_session_trace", 2, 1, 999, 0, 0)
        self.call("trace_writer", 0, 0)
        self.assertEqual(int(self.writes[-1].split()[1], 16), 152)

    def test_instance_limit_stops_producer_and_writer(self):
        self.call("gobt_session_trace_start", 0x06060010)
        word(self.cpu, "trace_sequence", 1023)
        self.call("gobt_session_trace", 2, 1, 0, 0, 0)
        self.call("gobt_session_trace", 2, 1, 0, 0, 0)
        self.assertEqual(self.value("trace_sequence"), 1024)
        self.call("trace_writer", 0, 0)
        self.assertEqual((len(self.writes), self.closes), (2, [9]))
        self.assertEqual(self.value("trace_enabled"), 0)

    def test_oversized_file_rotates_before_writing(self):
        self.file_length = 256 * 1024
        self.call("gobt_session_trace_start", 0x06060010)
        self.call("trace_writer", 0, 0)
        self.assertEqual(len(self.opens), 2)
        self.assertTrue(self.opens[1] & 0x400)  # PSP_O_TRUNC
        self.assertEqual(self.closes, [9])

    def test_io_failures_disable_tracing_without_affecting_input(self):
        for opening in (True, False):
            with self.subTest(opening=opening):
                self.setUp()
                self.open_error, self.write_error = opening, not opening
                self.call("gobt_session_trace_start", 0x06060010)
                self.call("trace_writer", 0, 0)
                self.assertEqual(self.value("trace_enabled"), 0)

    def test_stop_is_bounded_and_callbacks_cannot_enqueue_afterward(self):
        self.call("gobt_session_trace_start", 0x06060010)
        self.call("gobt_session_trace_stop")
        count = self.value("trace_count")
        self.call("gobt_session_trace", 2, 1, 0, 0, 0)
        self.assertEqual(self.value("trace_count"), count)
        self.assertEqual(self.value("trace_thread"), 20)  # not force killed
        self.call("trace_writer", 0, 0)
        self.assertEqual(self.closes, [9])


if __name__ == "__main__":
    unittest.main()
