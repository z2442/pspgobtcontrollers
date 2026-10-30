"""Execute event-driven input scheduling, burst fairness and CRC validation."""
import struct
import unittest
import zlib

from unicorn import UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import SYMBOLS, machine, word
from test_reconnect_execution import Session
from test_install_execution import Installation


class InputSession(Session):
    def __init__(self, case):
        self.signals, self.waits, self.delays, self.samples = [], [], [], []
        self.module_lookups, self.tokens = 0, 0
        self.inject_before_wait = None
        super().__init__(case)
        self.invoke("worker", 0, 0)  # Existing reconnect/enable path.
        word(self.cpu, "input_sema", 9)
        word(self.cpu, "active_source", 0)
        self.delays.clear()
        self.module_lookups = 0

    def observe(self, m, address, size, user):
        a, b, c = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0,
                                          reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2)]
        returns = {}
        if address == SYMBOLS["sceKernelSignalSema"]:
            self.case.assertEqual((a, b, m.reg_read(reg.UC_MIPS_REG_K1)), (9, 1, 0))
            self.signals.append(self.read_word("callback_depth"))
            result = 0 if not self.tokens else 0x800201AD
            self.tokens = 1  # A binary notification, never a packet count.
            returns[address] = result
        if address == SYMBOLS["sceKernelWaitSema"]:
            timeout = struct.unpack("<I", m.mem_read(c, 4))[0]
            self.case.assertEqual((a, b, timeout), (9, 1, 50000))
            self.waits.append(timeout)
            if self.inject_before_wait:
                # Complete data arrives after the worker's final empty check,
                # before the kernel wait. Its pending signal must be retained.
                packet = self.inject_before_wait
                self.inject_before_wait = None
                self.put_queued(packet)
                self.tokens = 1
            if self.tokens:
                self.tokens = 0
                returns[address] = 0
            else:
                m.emu_stop()
                return
        if address == SYMBOLS["gobt_sink_submit_state"]:
            self.samples.append(struct.unpack("<I", m.mem_read(a, 4))[0])
            returns[address] = 0
        if address == SYMBOLS["sceKernelFindModuleByName"]:
            self.module_lookups += 1
        if address == SYMBOLS["sceKernelDelayThread"]:
            self.delays.append(a)
        super().observe(m, address, size, user)
        if address in returns:
            m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
            m.reg_write(reg.UC_MIPS_REG_V1, 0)
            m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

    def put_queued(self, payload, generation=None):
        tail = self.read_word("tail")
        header = struct.pack("<5IiQ", 0, generation if generation is not None else
                             self.read_word("generations"), 1, len(payload), 0,
                             len(payload), self.clock)
        self.cpu.mem_write(SYMBOLS["queue"] + tail * 160, header + payload)
        word(self.cpu, "tail", (tail + 1) % 16)
        word(self.cpu, "queued", self.read_word("queued") + 1)

    def capture(self, payload):
        self.cpu.mem_write(0x0A608000, payload)
        self.cpu.reg_write(reg.UC_MIPS_REG_SP, 0x0A60F000)
        self.cpu.reg_write(reg.UC_MIPS_REG_RA, 0x0A700F00)
        for r, value in zip((reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                             reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3,
                             reg.UC_MIPS_REG_T0), (0, len(payload), 0x0A608000, 1, 0)):
            self.cpu.reg_write(r, value)
        self.cpu.emu_start(SYMBOLS["capture"], 0x0A700F00, count=20000)


def full_report(pressed=False):
    report = bytearray(78)
    report[0], report[2:6], report[9] = 0x31, bytes([128] * 4), 8 | (0x20 if pressed else 0)
    struct.pack_into("<I", report, 74, zlib.crc32(b"\xA1" + report[:74]))
    return b"\xA1" + report


class InputScheduling(unittest.TestCase):
    def test_idle_blocks_on_notification_with_fifty_ms_maintenance_timeout(self):
        s = InputSession(self)
        s.invoke("worker", 0, 0)
        self.assertEqual((s.waits, s.delays, s.module_lookups), ([50000], [], 2))

    def test_notification_before_wait_is_retained_and_decoded_without_delay(self):
        s = InputSession(self)
        s.inject_before_wait = full_report(True)
        s.invoke("worker", 0, 0)
        self.assertEqual(s.samples, [1 << 14])
        self.assertEqual((s.delays, len(s.waits), s.read_word("queued")), ([], 2, 0))

    def test_burst_preserves_press_release_order_and_bounds_high_priority_work(self):
        s = InputSession(self)
        for i in range(16):
            s.put_queued(full_report(not i % 2))
        s.invoke("worker", 0, 0)
        self.assertEqual(s.samples, [1 << 14, 0] * 4)
        self.assertEqual((s.read_word("queued"), s.module_lookups, s.delays), (8, 2, [1000]))
        s.invoke("worker", 0, 0)
        self.assertEqual(s.samples, [1 << 14, 0] * 8)
        self.assertEqual((s.read_word("queued"), s.module_lookups, s.delays), (0, 4, [1000, 1000]))

    def test_split_report_survives_batch_boundary_without_coalescing_fragments(self):
        s = InputSession(self)
        for _ in range(7):
            s.put_queued(full_report(False))
        packet = full_report(True)
        s.put_queued(packet[:64])
        s.put_queued(packet[64:])
        s.invoke("worker", 0, 0)
        self.assertEqual(s.samples, [0] * 7)
        s.invoke("worker", 0, 0)
        self.assertEqual(s.samples, [0] * 7 + [1 << 14])
        self.assertEqual(s.read_word("queued"), 0)

    def test_burst_rejects_corruption_expired_packets_and_old_generations(self):
        s = InputSession(self)
        damaged = bytearray(full_report(True))
        damaged[20] ^= 1
        s.put_queued(bytes(damaged))
        s.put_queued(full_report(True), s.read_word("generations") - 1)
        s.put_queued(full_report(True))
        s.clock += 250000
        s.put_queued(full_report(False))
        s.invoke("worker", 0, 0)
        self.assertEqual(s.samples, [0])

    def test_complete_report_replaces_partial_assembly_and_rejects_bad_crc(self):
        s = InputSession(self)
        packet = full_report(False)
        s.put_queued(packet[:64])
        s.put_queued(full_report(True))
        damaged = bytearray(packet)
        damaged[20] ^= 1
        s.put_queued(bytes(damaged))
        s.put_queued(packet[64:])  # Orphan remainder must not revive old data.
        s.invoke("worker", 0, 0)
        self.assertEqual(s.samples, [1 << 14])

    def test_callbacks_notify_only_after_sony_reposts_and_restore_caller_k1(self):
        s = InputSession(self)
        s.cpu.reg_write(reg.UC_MIPS_REG_K1, 0x12340000)
        s.invoke("callback_enter")
        s.invoke("callback_enter")
        s.capture(full_report(True))
        self.assertEqual(s.signals, [])
        self.assertEqual(s.invoke("callback_leave", 17), 17)
        self.assertEqual(s.signals, [])
        self.assertEqual(s.invoke("callback_leave", 42), 42)
        self.assertEqual(s.signals, [0])
        self.assertEqual(s.cpu.reg_read(reg.UC_MIPS_REG_K1), 0x12340000)
        s.invoke("worker", 0, 0)
        self.assertEqual(s.samples, [1 << 14])

    def test_semaphore_saturation_keeps_all_packets_and_one_wake_token(self):
        s = InputSession(self)
        for pressed in (True, False, True):
            s.capture(full_report(pressed))
        self.assertEqual((len(s.signals), s.tokens, s.read_word("queued")), (3, 1, 3))
        s.invoke("worker", 0, 0)
        self.assertEqual(s.samples, [1 << 14, 0, 1 << 14])
        self.assertEqual(s.delays, [])

    def test_native_close_and_shutdown_wake_an_idle_worker(self):
        s = InputSession(self)
        s.invoke("native_close_hid", 1)
        self.assertTrue(s.signals)
        self.assertEqual(s.read_word("pending_closures"), 2)
        before = len(s.signals)
        s.invoke("gobt_shutdown_input_bridge", 0)
        self.assertEqual(len(s.signals), before + 1)

    def test_priority_and_semaphore_lifetime_include_start_failure_paths(self):
        for failure in (None, "semaphore", "create", "start"):
            with self.subTest(failure=failure):
                s = Installation(self)
                if failure:
                    setattr(s, {"semaphore": "sema_result", "create": "create_result",
                                "start": "start_result"}[failure], 0x80020190)
                result = s.call("gobt_start_input_bridge", 0x06060010)
                self.assertEqual(result, 0x80020190 if failure else 0)
                self.assertEqual(s.read_word("worker_started"), int(not failure))
                if failure != "semaphore":
                    self.assertEqual(s.priorities, [0x10])
                self.assertEqual(s.sema_deleted, [10] if failure in ("create", "start") else [])
                self.assertEqual(len(s.unregistered), int(bool(failure)))
                if not failure:
                    # A timed-out join must retain the semaphore and thread.
                    s.wait_result = 0x800201A8
                    s.call("gobt_shutdown_input_bridge", 1)
                    self.assertEqual((s.sema_deleted, s.deleted), ([], []))
                    s.wait_result = 0
                    s.call("gobt_shutdown_input_bridge", 1)
                    self.assertEqual((s.sema_deleted, s.deleted), ([10], [8]))
                    s.call("gobt_shutdown_input_bridge", 1)
                    self.assertEqual(s.sema_deleted, [10])

    def test_crc_matches_zlib_and_is_bounded_on_the_actual_mips_build(self):
        cpu, counter = machine(), [0]
        def count(m, address, size, user):
            counter[0] += 1
        cpu.hook_add(UC_HOOK_CODE, count)
        for prefix in (0, 0xA1, 0xA2, 255):
            for length in (0, 1, 10, 74, 78, 128):
                data = bytes((i * 137 + length) & 255 for i in range(length))
                cpu.mem_write(0x0A608000, data)
                cpu.reg_write(reg.UC_MIPS_REG_SP, 0x0A60F000)
                cpu.reg_write(reg.UC_MIPS_REG_RA, 0x0A700F00)
                for r, value in zip((reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                                     reg.UC_MIPS_REG_A2), (prefix, 0x0A608000, length)):
                    cpu.reg_write(r, value)
                counter[0] = 0
                cpu.emu_start(SYMBOLS["ds_crc32"], 0x0A700F00, count=20000)
                self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), zlib.crc32(bytes([prefix]) + data))
                self.assertLess(counter[0], (length + 1) * 30 + 50)


if __name__ == "__main__":
    unittest.main()
