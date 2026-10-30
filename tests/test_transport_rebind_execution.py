"""Reproduce independent CDC replacement while the manager stays loaded."""
import struct
import unittest
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import SYMBOLS, word
from test_install_execution import Installation
from test_reconnect_execution import Session

SITES = (0xA22C, 0x9F40, 0xA1E4, 0xA1FC, 0x18D0, 0x2028, 0xA13C, 0xA24, 0xDABC, 0x2880)


class TransportRebind(unittest.TestCase):
    def snapshot(self, s):
        return [bytes(s.cpu.mem_read(s.physical(s.bsbase + x), 4)) for x in SITES]

    def test_new_worker_waits_for_initial_binding_to_publish_its_contexts(self):
        s = Installation(self)
        word(s.cpu, "binding", 1)
        word(s.cpu, "contexts", s.bsbase + 0xDE78)
        word(s.cpu, "wide_active", 1)
        s.call("worker", 0, 0)
        self.assertEqual(s.read_word("contexts"), s.bsbase + 0xDE78)
        self.assertEqual(s.read_word("wide_active"), 1)
        self.assertEqual(s.read_word("installed"), 0)

    def test_transport_disappears_then_reloads_with_same_manager_and_new_uid(self):
        for game in (False, True):
            s = Installation(self, game)
            originals = self.snapshot(s)
            self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
            patched = self.snapshot(s)
            s.cdc_available = False
            s.call("worker", 0, 0)
            self.assertEqual(s.read_word("installed"), 0)
            self.assertEqual(self.snapshot(s), originals)
            self.assertEqual(s.read_word("shutdown_done"), 0)  # a rebind, not final retirement
            s.cdc_available = True
            s.cpu.mem_write(s.cdc + 44, struct.pack("<I", 8))
            s.call("worker", 0, 0)
            self.assertEqual(s.read_word("installed"), 1)
            self.assertEqual(s.read_word("provider_uid"), 7)
            self.assertEqual(s.read_word("cdc_provider_uid"), 8)
            self.assertEqual(self.snapshot(s), patched)
            self.assertEqual(len(s.created), 1)  # preserve the existing worker

    def test_transport_replacement_without_absent_poll_is_detected(self):
        s = Installation(self)
        self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
        s.cpu.mem_write(s.cdc + 44, struct.pack("<I", 8))
        s.call("worker", 0, 0)
        self.assertEqual((s.read_word("installed"), s.read_word("cdc_provider_uid")), (1, 8))

    def test_rebound_connected_session_cancels_old_read_and_sends_fresh_initialization(self):
        s = Session(self)
        s.invoke("worker", 0, 0)
        self.assertEqual((s.starts, len(s.writes)), ([1], 1))
        word(s.cpu, "worker_started", 1)
        s.cpu.mem_write(s.cdc + 44, struct.pack("<I", 8))
        rebinds = []
        def bind(m, address, _, __):
            if address == SYMBOLS["bind_provider"]:
                rebinds.append(1)
                word(m, "installed", 1)
                word(m, "cdc_provider_uid", 8)
                m.reg_write(reg.UC_MIPS_REG_V0, 0)
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))
        s.cpu.hook_add(UC_HOOK_CODE, bind)
        s.invoke("worker", 0, 0)
        self.assertEqual(rebinds, [1])
        self.assertEqual(s.read_word("recover_devices"), 2)
        s.invoke("worker", 0, 0)
        self.assertEqual((s.stops, s.starts, len(s.writes)), ([1], [1, 1], 2))
        self.assertEqual(s.closes, [])

    def test_executing_callback_delays_restoration_and_rebind(self):
        s = Installation(self)
        self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
        patched = self.snapshot(s)
        word(s.cpu, "callback_depth", 1)
        s.cpu.mem_write(s.cdc + 44, struct.pack("<I", 8))
        s.call("worker", 0, 0)
        self.assertEqual(s.read_word("cdc_provider_uid"), 7)
        self.assertEqual(self.snapshot(s), patched)
        word(s.cpu, "callback_depth", 0)
        s.call("worker", 0, 0)
        self.assertEqual(s.read_word("cdc_provider_uid"), 8)

    def test_relocated_transport_waits_for_native_import_relink_and_binds_new_addresses(self):
        s = Installation(self)
        self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
        capture = Path("pspgobt/sceUsbBSMCDC_Driver-03F3DF17.text.bin").read_bytes()
        s.cdcbase = 0x881FA000
        s.cpu.mem_write(s.physical(s.cdcbase), capture)
        s.cpu.mem_write(s.cdc + 44, struct.pack("<I", 8))
        s.cpu.mem_write(s.cdc + 108, struct.pack("<II", s.cdcbase, len(capture)))
        s.call("worker", 0, 0)
        self.assertEqual(s.read_word("installed"), 0)  # manager still imports the old CDC
        s.cpu.mem_write(s.physical(s.bsbase + 0xCCC0), struct.pack(
            "<II", 0x08000000 | (((s.cdcbase + 0x1514) >> 2) & 0x03FFFFFF), 0))
        s.call("worker", 0, 0)
        self.assertEqual(s.read_word("installed"), 1)
        self.assertEqual(s.read_word("original_submit"), s.cdcbase + 0x1514)
        self.assertEqual(s.read_word("write_hid"), s.cdcbase + 0x1640)
        self.assertEqual(s.read_word("cdc_read_states"), s.cdcbase + 0x23A0)

    def test_surviving_manager_context_and_dma_ownership_are_retained_during_transport_gap(self):
        s = Installation(self)
        self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
        context = s.bsbase + 0xDE78
        s.cpu.mem_write(s.physical(context), struct.pack("<IHH", 2, 1, 1))
        word(s.cpu, "wide_active", 1)
        word(s.cpu, "wide_native", 0x88248000)
        word(s.cpu, "discovery_bypassed", 1)
        word(s.cpu, "prepared_devices", 2)
        s.cdc_available = False
        s.call("worker", 0, 0)
        self.assertEqual(s.read_word("contexts"), context)
        self.assertEqual(s.read_word("wide_active"), 1)
        self.assertEqual(s.read_word("wide_native"), 0x88248000)
        self.assertEqual(s.read_word("discovery_bypassed"), 1)
        self.assertEqual(s.read_word("prepared_devices"), 0)
        self.assertEqual(s.read_word("reads_ready"), 0)
        self.assertEqual(s.read_word("recover_devices"), 2)  # rebuild, even if a native read looks active
        # A manager replacement during that gap must discard old context state.
        s.cpu.mem_write(s.module + 44, struct.pack("<I", 9))
        s.call("worker", 0, 0)
        self.assertEqual(s.read_word("contexts"), 0)
        self.assertEqual(s.read_word("wide_active"), 0)
        self.assertEqual(s.read_word("recover_devices"), 0)

    def test_foreign_manager_patch_is_preserved_when_transport_changes(self):
        s = Installation(self)
        self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
        foreign = struct.pack("<I", 0x0C102030)
        s.cpu.mem_write(s.physical(s.bsbase + SITES[0]), foreign)
        s.cpu.mem_write(s.cdc + 44, struct.pack("<I", 8))
        s.call("worker", 0, 0)
        self.assertEqual(s.read_word("installed"), 0)  # new guard correctly refuses this image
        self.assertEqual(bytes(s.cpu.mem_read(s.physical(s.bsbase + SITES[0]), 4)), foreign)


if __name__ == "__main__":
    unittest.main()
