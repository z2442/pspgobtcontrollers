"""Exercise native disconnect dispatch and ordered session teardown in MIPS."""
import struct
import unittest

from unicorn import UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import machine, SYMBOLS, word
from test_reconnect_execution import Session
from test_install_execution import Installation


class Sessions(unittest.TestCase):
    def connected(self):
        s = Session(self)
        s.invoke("worker", 0, 0)
        self.assertEqual((s.starts, len(s.writes)), ([1], 1))
        return s

    def test_settings_disconnect_blocks_recovery_before_native_cleanup_event(self):
        s = self.connected()
        word(s.cpu, "active_source", 0)
        token = s.invoke("gobt_input_bridge_disconnecting", 1)
        self.assertNotEqual(token, 0)
        self.assertEqual(s.read_word("active_source"), 0xFFFFFFFF)
        s.clock = 3000000  # watchdog would otherwise rebuild the context
        s.invoke("worker", 0, 0)
        self.assertEqual((s.stops, s.starts, len(s.writes)), ([], [1], 1))
        s.invoke("gobt_input_bridge_disconnect_result", 1, token, 0)
        self.assertEqual(s.read_word("disconnect_requests"), 2)
        self.assertEqual(s.invoke("native_close_hid", 1), 0)
        self.assertEqual(s.stops, [])  # manager thread does not cancel
        self.assertEqual(s.read_word("closing_devices"), 2)
        s.flags = 0
        s.invoke("worker", 0, 0)
        self.assertEqual((s.stops, s.starts, s.closes), ([1], [1], []))
        self.assertEqual(s.read_word("closing_devices"), 0)
        self.assertEqual(s.read_word("wide_active"), 0)
        s.flags, s.clock = 0x202, 4000000
        s.invoke("native_setup_event", 1)
        s.invoke("worker", 0, 0)
        self.assertEqual((s.starts, len(s.writes)), ([1, 1], 2))

    def test_cancellation_repost_is_refused_during_manager_disconnect(self):
        s = self.connected()
        callback = 0x0A700900
        word(s.cpu, "original_receive", callback)
        s.invoke("native_close_hid", 1)
        # Sony's callback re-enters this submit hook while cancellation runs.
        for r, value in zip((reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                             reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3, reg.UC_MIPS_REG_T0),
                            (2, s.data, 64, callback, s.context)):
            s.cpu.reg_write(r, value)
        s.cpu.reg_write(reg.UC_MIPS_REG_RA, 0x0A700F00)
        s.cpu.reg_write(reg.UC_MIPS_REG_SP, 0x0A60F000)
        # original_submit is deliberately null: a repost would fault.
        s.cpu.emu_start(SYMBOLS["submit_next"], 0x0A700F00, count=10000)
        self.assertEqual(s.cpu.reg_read(reg.UC_MIPS_REG_V0), 0xFFFFFFFF)
        self.assertEqual(s.read_word("reads_ready"), 0)

    def test_old_request_result_cannot_clear_new_closing_session(self):
        s = self.connected()
        token = s.invoke("gobt_input_bridge_disconnecting", 1)
        s.invoke("native_close_hid", 1)
        s.invoke("gobt_input_bridge_disconnect_result", 1, token, 0x802F0134)
        self.assertEqual(s.read_word("closing_devices"), 2)
        self.assertEqual(s.read_word("pending_closures"), 2)

    def test_failed_disconnect_releases_intent_and_allows_input_recovery(self):
        s = self.connected()
        token = s.invoke("gobt_input_bridge_disconnecting", 1)
        s.invoke("gobt_input_bridge_disconnect_result", 1, token, 0x802F0134)
        self.assertEqual(s.read_word("disconnect_requests"), 0)
        s.clock = 300000
        s.invoke("worker", 0, 0)
        self.assertEqual((s.stops, len(s.writes)), ([], 2))

    def test_already_disconnected_success_still_releases_old_context(self):
        s = self.connected()
        token = s.invoke("gobt_input_bridge_disconnecting", 1)
        s.flags = 0
        s.invoke("gobt_input_bridge_disconnect_result", 1, token, 0)
        self.assertEqual(s.read_word("pending_closures"), 2)
        s.invoke("worker", 0, 0)
        self.assertEqual((s.stops, s.read_word("closing_devices")), ([1], 0))

    def test_close_failure_retains_ownership_and_blocks_new_setup(self):
        s = self.connected()
        s.invoke("native_close_hid", 1)
        s.stop_error = 0x802F0208
        for attempt in range(4):
            s.clock = 1000000 * attempt + 1000
            s.invoke("worker", 0, 0)
        self.assertEqual(s.stops, [1, 1, 1])
        self.assertEqual((s.starts, len(s.writes)), ([1], 1))
        self.assertEqual(s.read_word("closing_devices"), 2)
        self.assertEqual(s.read_word("wide_active"), 1)
        self.assertEqual(s.read_word("wide_native"), s.data)

    def test_disconnect_arriving_during_setup_finishes_old_setup_then_closes(self):
        s, interrupted = Session(self), []

        def event(m, address, _, __):
            if address != s.setup or interrupted:
                return
            interrupted.append(True)
            # Pause after the mocked setup, preserving its worker stack.
            m.emu_stop()

        s.cpu.hook_add(UC_HOOK_CODE, event)
        s.invoke("worker", 0, 0)
        saved = s.cpu.context_save()
        s.invoke("native_close_hid", 1)
        self.assertEqual(s.stops, [])
        s.cpu.context_restore(saved)
        s.cpu.emu_start(s.cpu.reg_read(reg.UC_MIPS_REG_PC), 0x0A700F00, count=200000)
        self.assertEqual((s.stops, s.writes), ([], []))
        s.flags = 0
        s.invoke("worker", 0, 0)
        self.assertEqual(s.stops, [1])

    def test_native_cleanup_for_other_controller_preserves_original_result(self):
        s = Session(self)
        s.stop_error = 0x802F0208
        self.assertEqual(s.invoke("native_close_hid", 1), s.stop_error)
        self.assertEqual(s.stops, [1])
        self.assertEqual(s.read_word("pending_closures"), 0)

    def test_xmb_disconnect_wrapper_preserves_result_k1_and_call_order(self):
        for result in (0, 0x802F0134):
            cpu, calls = machine(), []
            native = 0x0A700100
            cpu.mem_write(SYMBOLS["apis"] + 5 * 24 + 12, struct.pack("<I", native))
            cpu.reg_write(reg.UC_MIPS_REG_K1, 0x12340000)

            def observe(m, address, _, __):
                if address not in (SYMBOLS["gobt_input_bridge_disconnecting"], native,
                                    SYMBOLS["gobt_input_bridge_disconnect_result"]):
                    return
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), 1)
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_K1), 0x12340000)
                calls.append(address)
                value = 19 if address == SYMBOLS["gobt_input_bridge_disconnecting"] else result
                if address == SYMBOLS["gobt_input_bridge_disconnect_result"]:
                    self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A1), 19)
                    self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A2), result)
                m.reg_write(reg.UC_MIPS_REG_V0, value)
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

            cpu.hook_add(UC_HOOK_CODE, observe)
            cpu.reg_write(reg.UC_MIPS_REG_A0, 1)
            cpu.emu_start(SYMBOLS["pspgobtTraceDisconnect"], 0x0A700F00, count=10000)
            self.assertEqual(calls, [SYMBOLS["gobt_input_bridge_disconnecting"], native,
                                      SYMBOLS["gobt_input_bridge_disconnect_result"]])
            self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), result)

    def test_cleanup_pointer_is_guarded_and_restored_for_both_module_layouts(self):
        for game in (False, True):
            s = Installation(self, game)
            pointer = s.physical(s.bsbase + 0xDABC)
            original = bytes(s.cpu.mem_read(pointer, 4))
            self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
            self.assertEqual(struct.unpack("<I", s.cpu.mem_read(pointer, 4))[0], SYMBOLS["native_close_hid"])
            s.call("gobt_shutdown_input_bridge", 0)
            s.call("worker", 0, 0)
            self.assertEqual(bytes(s.cpu.mem_read(pointer, 4)), original)
            for offset in (0xDABC, 0x26CC, 0x26D0, 0x26D4, 0x26DC, 0x2718, 0x271C, 0x2720):
                s = Installation(self, game)
                site = s.physical(s.bsbase + offset)
                data = bytes(s.cpu.mem_read(site, 4))
                s.cpu.mem_write(site, bytes([data[0] ^ 1]) + data[1:])
                self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0xFFFFFFFE)

    def test_connect_waits_for_cleanup_and_never_enters_sony_while_closing(self):
        for completes in (True, False):
            cpu, delays, calls = machine(), [], []
            native = 0x0A700100
            cpu.mem_write(SYMBOLS["apis"] + 3 * 24 + 12, struct.pack("<I", native))
            word(cpu, "closing_devices", 2)

            def observe(m, address, _, __):
                returns = {SYMBOLS["gobt_input_bridge_prepare"]: 1,
                           SYMBOLS["gobt_input_bridge_connected"]: 0,
                           SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                           SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                           SYMBOLS["gobt_log"]: 0}
                if address == SYMBOLS["sceKernelDelayThread"]:
                    self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), 5000)
                    delays.append(address)
                    if completes and len(delays) == 2:
                        word(m, "closing_devices", 0)
                    returns[address] = 0
                if address == native:
                    self.assertTrue(completes)
                    self.assertEqual(len(delays), 2)
                    self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A1), 0)
                    calls.append(address)
                    returns[address] = 0
                if address in returns:
                    m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                    m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

            cpu.hook_add(UC_HOOK_CODE, observe)
            cpu.reg_write(reg.UC_MIPS_REG_A0, 1)
            cpu.reg_write(reg.UC_MIPS_REG_A1, 1)
            cpu.emu_start(SYMBOLS["pspgobtTraceConnect"], 0x0A700F00, count=100000)
            self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), 0 if completes else 0xFFFFFFFF)
            self.assertEqual((len(delays), len(calls)), (2, 1) if completes else (50, 0))

    def test_shutdown_preserves_a_foreign_cleanup_pointer(self):
        s = Installation(self)
        self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
        pointer, foreign = s.physical(s.bsbase + 0xDABC), struct.pack("<I", 0x88051000)
        s.cpu.mem_write(pointer, foreign)
        s.call("gobt_shutdown_input_bridge", 0)
        s.call("worker", 0, 0)
        self.assertEqual(bytes(s.cpu.mem_read(pointer, 4)), foreign)

    def test_captured_manager_dispatch_defers_selected_teardown_and_preserves_context(self):
        for game in (False, True):
            s = Installation(self, game)
            self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
            context = s.bsbase + 0xDE78
            s.cpu.mem_write(s.physical(context), struct.pack("<IHH", 2, 1, 1))
            word(s.cpu, "discovery_bypassed", 1)
            event = 0x0A600400
            s.cpu.mem_write(event, struct.pack("<HHH", 1, 0, 2))
            s.cpu.reg_write(reg.UC_MIPS_REG_S1, 1)  # HID profile bit
            s.cpu.reg_write(reg.UC_MIPS_REG_S2, event)
            s.cpu.reg_write(reg.UC_MIPS_REG_SP, 0x0A60F000)
            # Execute the actual Sony table lookup, jalr, slot argument in
            # its delay slot, and unconditional continuation after cleanup.
            s.cpu.emu_start(s.bsbase + 0x26CC, s.bsbase + 0x26EC, count=10000)
            self.assertEqual(s.read_word("closing_devices"), 2)
            self.assertEqual(s.read_word("pending_closures"), 2)
            self.assertEqual(bytes(s.cpu.mem_read(s.physical(context), 8)), struct.pack("<IHH", 2, 1, 1))
            self.assertEqual(s.cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)


if __name__ == "__main__":
    unittest.main()
