"""Execute Switch Pro setup and input through the compiled PSP bridge."""
import struct
import unittest

from unicorn import mips_const as reg
from test_bridge_execution import SYMBOLS, word
from test_reconnect_execution import Session
import test_reconnect_execution as reconnect
from test_input_scheduling_execution import InputSession, full_report


def stick(x=2048, y=2048):
    return bytes([x & 255, (x >> 8) | ((y & 15) << 4), y >> 4])


def controls(button=0, x=2048, y=2048):
    report = bytearray(49)
    report[0], report[3] = 0x30, button
    report[6:9], report[9:12] = stick(x, y), stick(2000, 1900)
    return b"\xA1" + report


def reply(command, address=0, data=b"", ack=0x80):
    report = bytearray(49)
    report[0], report[13], report[14] = 0x21, ack, command
    if command == 0x10:
        struct.pack_into("<IB", report, 15, address, len(data))
        report[20:20 + len(data)] = data
    return b"\xA1" + report


class SwitchSession(Session):
    put_queued = InputSession.put_queued

    def __init__(self, case):
        self.samples = []
        super().__init__(case)
        word(self.cpu, "active_source", 0)

    def observe(self, m, address, size, user):
        a, b, c = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2)]
        result = None
        if address == self.info:
            self.reads.append(a)
            record = bytearray(80)
            struct.pack_into("<I", record, 0, 80)
            record[6:34] = "Pro Controller".encode("utf-16le")
            struct.pack_into("<H", record, 72, 0x402)
            m.mem_write(b, bytes(record))
            result = self.query_error
        elif address == self.writer:
            self.case.assertEqual((a, b % 64, c), (2, 0, 50))
            packet = bytes(m.mem_read(b, c))
            self.case.assertEqual(packet[:2], b"\xA2\x01")
            self.case.assertEqual(packet[3:11], b"\x00\x01\x40\x40" * 2)
            self.writes.append(packet)
            result = c if self.write_error == 79 else self.write_error
        elif address == SYMBOLS["gobt_sink_submit_state"]:
            self.samples.append(bytes(m.mem_read(a, 10)))
            result = 0
        else:
            super().observe(m, address, size, user)
        if result is not None:
            m.reg_write(reg.UC_MIPS_REG_V0, result)
            m.reg_write(reg.UC_MIPS_REG_V1, 0)
            m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

    def run_packet(self, packet):
        self.clock += 100000
        self.put_queued(packet)
        self.invoke("worker", 0, 0)

    def complete_setup(self):
        self.invoke("worker", 0, 0)
        word(self.cpu, "active_source", 0)
        self.case.assertEqual(self.writes[-1][11:13], b"\x03\x30")
        self.run_packet(reply(3))
        self.case.assertEqual(self.writes[-1][11:17], b"\x10\x3D\x60\x00\x00\x12")
        factory = stick(1000, 1000) + stick(2100, 2200) + stick(900, 900)
        factory += stick(2000, 1900) + stick(900, 900) + stick(1000, 1000)
        self.run_packet(reply(0x10, 0x603D, factory, 0x90))
        self.case.assertEqual(self.writes[-1][11:17], b"\x10\x10\x80\x00\x00\x16")
        self.run_packet(reply(0x10, 0x8010, bytes([255] * 22), 0x90))
        self.case.assertEqual(self.writes[-1][11:13], b"\x30\x01")
        self.run_packet(reply(0x30))
        self.case.assertEqual(self.samples, [])
        self.run_packet(controls(4, 2100, 2200))
        self.case.assertEqual(self.samples[-1], struct.pack("<I6B", 1 << 14, 128, 128, 128, 128, 0, 0))
        self.case.assertEqual([p[2] for p in self.writes], [0, 1, 2, 3])


class SwitchProExecution(unittest.TestCase):
    def test_setup_calibration_worker_and_native_sampler(self):
        s = SwitchSession(self)
        s.complete_setup()
        previous = len(s.writes)
        s.put_queued(controls(8, 3100, 3200))
        s.invoke("gobt_input_bridge_sample")
        self.assertEqual(s.samples[-1], struct.pack("<I6B", 1 << 13, 255, 0, 128, 128, 0, 0))
        self.assertEqual((s.read_word("queued"), len(s.writes)), (0, previous))
        for packet in (full_report(True), controls()[:-1], b"\xB1" + controls()[1:]):
            s.put_queued(packet)
        s.invoke("worker", 0, 0)
        self.assertEqual(len(s.samples), 2)
        self.assertEqual(len(s.writes), previous)
        s.put_queued(controls())
        s.clock += 250000
        s.invoke("gobt_input_bridge_sample")
        self.assertEqual(len(s.samples), 2)

    def test_saved_model_restores_protocol_without_metadata_and_resets_on_reconnect(self):
        s = SwitchSession(self)
        reconnect.Reconnect.paired_target(self, s, model=3)
        word(s.cpu, "get_info", 0xDEADBEEF)
        s.forbid_record_query = True  # Link settings only; no metadata query.
        s.invoke("worker", 0, 0)
        self.assertEqual(s.reads, [])
        self.assertEqual([command[0] for command in s.link_commands], [0x79, 0x24])
        self.assertEqual(s.writes[-1][2], 0)
        self.assertEqual(s.writes[-1][11:13], b"\x03\x30")
        s.flags = 0
        s.invoke("poll_links")
        s.cpu.mem_write(s.physical(s.context), bytes(48))
        s.cpu.mem_write(s.physical(s.states + 2 * 12 + 0x7C), bytes(12))
        s.flags = 0x202
        s.clock += 600000
        s.invoke("worker", 0, 0)
        self.assertEqual(s.writes[-1][2], 0)  # Fresh command sequence and setup.
        self.assertEqual(s.writes[-1][11:13], b"\x03\x30")
        self.assertEqual(len(s.writes), 2)

    def test_failed_writes_retry_with_new_sequence_and_lifecycle_guards(self):
        s = SwitchSession(self)
        s.write_error = 49  # Short completion must not initialize the stream.
        s.invoke("worker", 0, 0)
        self.assertEqual(struct.unpack("<I", s.cpu.mem_read(SYMBOLS["sources"] + 20, 4))[0], 0)
        for _ in range(2):
            s.clock += 100000
            s.invoke("worker", 0, 0)
        self.assertEqual([p[2] for p in s.writes], [0, 1, 2])
        s.clock += 100000
        s.invoke("worker", 0, 0)
        self.assertEqual(len(s.writes), 3)
        for name in ("suspended", "quiescing"):
            word(s.cpu, name, 1)
            s.put_queued(controls(4))
            s.invoke("gobt_input_bridge_sample")
            self.assertEqual(s.samples, [])
            word(s.cpu, name, 0)

    def test_setup_replies_are_left_for_worker_by_sampler(self):
        s = SwitchSession(self)
        s.complete_setup()
        s.put_queued(reply(0x30))
        s.put_queued(controls(4))
        s.invoke("gobt_input_bridge_sample")
        self.assertEqual(s.read_word("queued"), 2)
        s.invoke("worker", 0, 0)
        self.assertEqual(len(s.samples), 2)


if __name__ == "__main__":
    unittest.main()
