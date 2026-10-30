"""Run cooperative kernel-transition cleanup, hook retirement and fallbacks."""
import struct
import unittest

from unicorn import UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import machine, SYMBOLS, word
from test_install_execution import Installation
from test_reconnect_execution import Session

SITES = (0xA22C, 0x9F40, 0xA1E4, 0xA1FC, 0x18D0, 0x2028, 0xA13C, 0xA24, 0x2880)


class Shutdown(unittest.TestCase):
    def test_kernel_reboot_callbacks_are_exported_with_standard_nids(self):
        cpu = machine()
        table = struct.unpack("<10I", cpu.mem_read(SYMBOLS["__syslib_exports"], 40))
        exported = dict(zip(table[:5], table[5:]))
        self.assertEqual(exported[0x2F064FA6], SYMBOLS["module_reboot_before"])
        self.assertEqual(exported[0xADF12745], SYMBOLS["module_reboot_phase"])

    def hooks(self, s):
        return [bytes(s.cpu.mem_read(s.physical(s.bsbase + site), 4)) for site in SITES]

    def test_worker_restores_only_its_own_hooks_and_is_joined_once(self):
        s = Installation(self, True)
        originals = self.hooks(s)
        self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
        foreign = struct.pack("<I", 0x0C102030)
        s.cpu.mem_write(s.physical(s.bsbase + SITES[0]), foreign)
        s.call("gobt_shutdown_input_bridge", 0)
        self.assertEqual(s.read_word("quiescing"), 1)
        s.call("worker", 0, 0)
        self.assertEqual(self.hooks(s), [foreign] + originals[1:])
        self.assertEqual(s.read_word("installed"), 0)
        s.call("gobt_shutdown_input_bridge", 1)
        s.call("gobt_shutdown_input_bridge", 0)
        self.assertEqual((s.waits, s.deleted), ([8], [8]))
        self.assertEqual(s.call("gobt_start_input_bridge", 0x06060010), 0xFFFFFFF7)
        self.assertEqual(s.call("native_setup_event", 1), 0xFFFFFFFF)

    def test_timeout_and_executing_callback_prevent_early_hook_retirement(self):
        s = Installation(self)
        self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
        installed = self.hooks(s)
        s.wait_result = 0x800201A8
        s.call("gobt_shutdown_input_bridge", 1)
        s.call("gobt_shutdown_input_bridge", 0)
        self.assertEqual(self.hooks(s), installed)
        self.assertEqual(s.deleted, [])
        word(s.cpu, "callback_depth", 1)
        s.call("worker", 0, 0)
        s.call("gobt_shutdown_input_bridge", 0)
        self.assertEqual(self.hooks(s), installed)
        word(s.cpu, "callback_depth", 0)
        s.call("gobt_shutdown_input_bridge", 0)
        self.assertEqual(self.hooks(s), installed)  # worker has not reached cleanup yet
        s.call("worker", 0, 0)
        self.assertEqual(s.read_word("installed"), 0)

    def test_pending_staging_dma_prevents_retirement_until_driver_cancels_it(self):
        s = Installation(self)
        self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
        installed = self.hooks(s)
        slot = s.physical(s.cdcbase + 0x23A0 + 2 * 12 + 0x7C)
        s.cpu.mem_write(slot, struct.pack("<III", SYMBOLS["receive_wide"], SYMBOLS["receive_buffers"], 0))
        s.call("gobt_shutdown_input_bridge", 0)
        s.call("worker", 0, 0)
        self.assertEqual(self.hooks(s), installed)
        s.cpu.mem_write(slot, bytes(12))
        s.call("gobt_shutdown_input_bridge", 0)
        self.assertEqual(s.read_word("installed"), 0)

    def test_cooperative_worker_cancels_owned_local_context_without_disconnect(self):
        s = Session(self)
        s.invoke("worker", 0, 0)
        word(s.cpu, "cdc_provider_uid", 7)
        word(s.cpu, "worker_id", 8)
        word(s.cpu, "worker_started", 1)
        s.invoke("gobt_shutdown_input_bridge", 0)
        s.invoke("worker", 0, 0)
        self.assertEqual((s.stops, s.closes), ([1], []))
        self.assertEqual(s.read_word("installed"), 0)
        self.assertEqual(s.read_word("queued"), 0)
        self.assertEqual(len(s.writes), 1)  # no new controller output on exit

    def test_reboot_exports_dispatch_before_and_phase_shutdown(self):
        for phase in (False, True):
            with self.subTest(phase=phase):
                cpu, calls = machine(), []

                def observe(m, address, _, __):
                    if address in (SYMBOLS["gobt_shutdown_input_bridge"], SYMBOLS["gobt_sink_shutdown"], SYMBOLS.get("gobt_session_trace_stop")):
                        calls.append((address, m.reg_read(reg.UC_MIPS_REG_A0)))
                        m.reg_write(reg.UC_MIPS_REG_V0, 0)
                        m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

                cpu.hook_add(UC_HOOK_CODE, observe)
                function = "module_reboot_phase" if phase else "module_reboot_before"
                cpu.emu_start(SYMBOLS[function], 0x0A700F00, count=10000)
                self.assertEqual(calls[0], (SYMBOLS["gobt_shutdown_input_bridge"], int(not phase)))
                self.assertEqual(calls[1][0], SYMBOLS["gobt_sink_shutdown"])
                self.assertEqual(struct.unpack("<I", cpu.mem_read(SYMBOLS["leaving"], 4))[0], 1)


if __name__ == "__main__":
    unittest.main()
