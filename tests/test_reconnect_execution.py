"""Run boot/reconnect orchestration with stored pairing and mocked transport."""
import struct
import unittest

from unicorn import UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import machine, SYMBOLS, word


class Session:
    def __init__(self, case):
        self.case, self.cpu = case, machine()
        self.cpu.mem_map(0x08200000, 0x10000)
        self.context, self.states, self.entry, self.data = 0x88201000, 0x88202000, 0x88203000, 0x88204000
        self.provider, self.cdc = 0x0A600000, 0x0A600200
        self.info, self.flags_api, self.setup, self.disconnect = 0x0A700100, 0x0A700200, 0x0A700300, 0x0A700400
        self.writer, self.did = 0x0A700500, 0x0A700600
        self.stop = 0x0A700700
        self.clock, self.flags, self.query_error, self.setup_error = 1000, 0x202, 0, 0
        self.matching = True
        self.reads, self.starts, self.writes, self.closes = [], [], [], []
        self.stops, self.stop_error = [], 0
        self.cpu.mem_write(self.provider + 44, struct.pack("<I", 7))
        self.cpu.mem_write(self.cdc + 44, struct.pack("<I", 7))
        self.cpu.mem_write(self.cdc + 108, struct.pack("<I", self.states - 0x23A0))
        self.cpu.mem_write(SYMBOLS["contexts"], struct.pack("<II", self.context, self.context + 48))
        for name, value in (("installed", 1), ("provider_uid", 7), ("cdc_provider_uid", 7), ("get_info", self.info),
                            ("connection_flags", self.flags_api), ("start_hid", self.setup),
                            ("disconnect_hid", self.disconnect), ("write_hid", self.writer),
                            ("stop_hid", self.stop), ("cdc_read_states", self.states)):
            word(self.cpu, name, value)
        self.cpu.hook_add(UC_HOOK_CODE, self.observe)

    @staticmethod
    def physical(pointer):
        return pointer & 0x1FFFFFFF if pointer >= 0x80000000 else pointer

    def read_word(self, name):
        return struct.unpack("<I", self.cpu.mem_read(SYMBOLS[name], 4))[0]

    def observe(self, m, address, _, __):
        args = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                                       reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3)]
        returns = {SYMBOLS["sceKernelFindModuleByName"]: self.provider,
                   SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                   SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                   SYMBOLS["sceKernelGetSystemTimeWide"]: self.clock,
                   SYMBOLS["gobt_log"]: 0, SYMBOLS["pspgobtDisconnect"]: 0}
        if address == SYMBOLS["sceKernelFindModuleByName"]:
            name = bytes(m.mem_read(args[0], 64)).split(b"\0", 1)[0]
            returns[address] = self.cdc if name == b"sceUsbBSMCDC_Driver" else self.provider
        if address == self.flags_api:
            returns[address] = self.flags if args[0] == 1 else 0
        if address == self.info:
            self.case.assertEqual(args[0], 1)
            self.reads.append(1)
            record = bytearray(80)
            struct.pack_into("<I", record, 0, 80)
            name = ("DualSense Wireless Controller" if self.matching else "Other Controller").encode("utf-16le")
            record[6:6 + len(name)] = name
            struct.pack_into("<H", record, 72, 0x402)
            m.mem_write(args[1], bytes(record))
            returns[address] = self.query_error
        if address == self.did:
            self.case.fail("Boot/reconnect must not launch remote DID discovery")
        if address == self.setup:
            self.case.assertEqual(args[0], 1)
            self.case.assertEqual(bool(self.read_word("prepared_devices") & 2), self.matching)
            self.starts.append(1)
            if not self.setup_error:
                m.mem_write(self.physical(self.context), struct.pack("<IHH", 2, 1, 1))
                m.mem_write(self.physical(self.context + 40), struct.pack("<I", self.entry))
                m.mem_write(self.physical(self.entry + 8), struct.pack("<I", self.data))
                word(m, "generations", self.read_word("generations") + 1)
                word(m, "reads_ready", 1)
                word(m, "wide_active", 1)
                word(m, "discovery_bypassed", int(self.matching))
                word(m, "wide_native", self.data)
                m.mem_write(self.physical(self.states + 2 * 12 + 0x7C), struct.pack(
                    "<III", SYMBOLS["receive_wide"], SYMBOLS["receive_buffers"], self.context))
            returns[address] = self.setup_error
        if address == self.writer:
            self.case.assertEqual((args[0], args[1] % 64, args[2]), (2, 0, 79))
            self.writes.append(bytes(m.mem_read(args[1], 79)))
            returns[address] = 79
        if address == self.disconnect:
            self.case.assertEqual(args[:3], [1, 2, 0])
            self.closes.append(1)
            returns[address] = 0
        if address == self.stop:
            self.case.assertEqual(args[0], 1)
            self.stops.append(1)
            if not self.stop_error:
                m.mem_write(self.physical(self.context), bytes(8))
                m.mem_write(self.physical(self.states + 2 * 12 + 0x7C), bytes(12))
            returns[address] = self.stop_error
        if address == SYMBOLS.get("memset"):
            m.mem_write(self.physical(args[0]), bytes([args[1] & 255]) * args[2])
            returns[address] = args[0]
        if address == SYMBOLS.get("memcpy"):
            m.mem_write(self.physical(args[0]), bytes(m.mem_read(self.physical(args[1]), args[2])))
            returns[address] = args[0]
        if address == SYMBOLS.get("memcmp"):
            returns[address] = int(bytes(m.mem_read(self.physical(args[0]), args[2])) !=
                                   bytes(m.mem_read(self.physical(args[1]), args[2])))
        if address == SYMBOLS["sceKernelDelayThread"]:
            m.emu_stop()  # Complete one idle worker pass.
        if address in returns:
            m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
            m.reg_write(reg.UC_MIPS_REG_V1, 0)
            m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

    def invoke(self, name, *args):
        self.cpu.reg_write(reg.UC_MIPS_REG_SP, 0x0A60F000)
        self.cpu.reg_write(reg.UC_MIPS_REG_RA, 0x0A700F00)
        for r, value in zip((reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2), args):
            self.cpu.reg_write(r, value)
        self.cpu.emu_start(SYMBOLS[name], 0x0A700F00, count=200000)
        return self.cpu.reg_read(reg.UC_MIPS_REG_V0)


class Reconnect(unittest.TestCase):
    def paired_target(self, session, known=True, address=b"\x01\x02\x03\x04\x05\x06", caps=0x402, slot=1):
        table = 0x88205000
        word(session.cpu, "paired_records", table)
        session.cpu.mem_write(session.physical(table + 0x24), struct.pack("<HHH", slot, caps, 0) + address)
        cache = struct.pack("<I", int(known)) + (b"\x01\x02\x03\x04\x05\x06" if known else bytes(6)) + bytes(6)
        session.cpu.mem_write(SYMBOLS["target_cache"], cache)

    def test_already_connected_known_peer_starts_without_metadata_queries(self):
        session = Session(self)
        self.paired_target(session)
        word(session.cpu, "get_info", 0xDEADBEEF)  # unreachable if cached attachment works
        session.invoke("worker", 0, 0)
        self.assertEqual(session.reads, [])
        self.assertEqual((len(session.starts), len(session.writes)), (1, 1))
        self.assertEqual(session.read_word("prepared_devices"), 2)

    def test_cached_address_does_not_select_a_changed_or_removed_pairing(self):
        for address, caps, slot in ((b"\x07" * 6, 0x402, 1),
                                    (b"\x01\x02\x03\x04\x05\x06", 0, 1),
                                    (b"\x01\x02\x03\x04\x05\x06", 0x402, 0)):
            with self.subTest(caps=caps, slot=slot):
                session = Session(self)
                self.paired_target(session, address=address, caps=caps, slot=slot)
                session.query_error = 0x802F0203
                session.invoke("worker", 0, 0)
                self.assertEqual(session.reads, [1])
                self.assertEqual(session.starts, [])

    def test_known_peer_reloads_context_from_previous_application_without_query(self):
        session = Session(self)
        self.paired_target(session)
        self.seed_old_context(session)
        word(session.cpu, "get_info", 0xDEADBEEF)
        session.invoke("worker", 0, 0)
        self.assertEqual((session.stops, session.starts, session.closes), ([1], [1], []))

    def test_first_metadata_selection_persists_peer_for_next_prx_instance(self):
        session = Session(self)
        self.paired_target(session, known=False)
        configurations = []

        def filesystem(m, address, _, __):
            args = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2)]
            returns = {}
            if address == SYMBOLS["sceIoOpen"]:
                path = bytes(m.mem_read(args[0], 96)).split(b"\0", 1)[0]
                self.assertTrue(path.endswith(b"/controllers.cfg"))
                returns[address] = 9
            if address == SYMBOLS["sceIoWrite"]:
                self.assertEqual((args[0], args[2]), (9, 32))
                configurations.append(bytes(m.mem_read(args[1], 32)))
                returns[address] = 32
            if address == SYMBOLS["sceIoClose"]:
                returns[address] = 0
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        session.cpu.hook_add(UC_HOOK_CODE, filesystem)
        session.invoke("worker", 0, 0)
        self.assertEqual(session.reads, [1])
        self.assertEqual(len(configurations), 1)
        next_instance = Session(self)
        self.paired_target(next_instance, known=False)
        next_instance.cpu.mem_write(0x0A600700, configurations[0])
        self.assertEqual(next_instance.invoke("gobt_target_decode", SYMBOLS["target_cache"], 0x0A600700, 32), 0)
        word(next_instance.cpu, "get_info", 0xDEADBEEF)
        next_instance.invoke("worker", 0, 0)
        self.assertEqual(next_instance.reads, [])
        self.assertEqual(len(next_instance.writes), 1)

    def test_silent_pending_read_recovers_without_bluetooth_disconnect(self):
        session = Session(self)
        session.invoke("worker", 0, 0)
        session.clock = 2000999
        session.invoke("worker", 0, 0)
        self.assertEqual(session.stops, [])
        session.clock = 2201000
        session.invoke("worker", 0, 0)
        self.assertEqual((len(session.stops), len(session.starts), len(session.writes)), (1, 2, 2))
        self.assertEqual(session.flags, 0x202)
        self.assertEqual(session.closes, [])

    def test_silent_stream_recovery_is_bounded(self):
        session = Session(self)
        session.invoke("worker", 0, 0)
        for clock in (2201000, 4401000, 6601000, 8801000, 11001000):
            session.clock = clock
            session.invoke("worker", 0, 0)
        self.assertEqual((len(session.stops), len(session.starts)), (3, 4))
        self.assertEqual(session.closes, [])

    def test_fresh_valid_report_resets_watchdog_budget_and_prevents_reset(self):
        session = Session(self)
        session.invoke("worker", 0, 0)
        session.cpu.mem_write(SYMBOLS["stream_recoveries"] + 4, struct.pack("<I", 3))
        session.clock = 1801000
        report = bytes([0xA1, 1, 128, 128, 128, 128, 8, 0, 0, 0, 0])
        packet = struct.pack("<6IQ", 0, session.read_word("generations"), 1,
                             len(report), 0, len(report), session.clock) + report
        session.cpu.mem_write(0x0A600800, packet)

        def sink(m, address, _, __):
            if address in (SYMBOLS["gobt_sink_start"], SYMBOLS["gobt_sink_submit_state"]):
                m.reg_write(reg.UC_MIPS_REG_V0, 0)
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        session.cpu.hook_add(UC_HOOK_CODE, sink)
        session.invoke("consume", 0x0A600800)
        self.assertEqual(struct.unpack("<Q", session.cpu.mem_read(SYMBOLS["last_report"], 8))[0], session.clock)
        self.assertEqual(struct.unpack("<I", session.cpu.mem_read(SYMBOLS["stream_recoveries"] + 4, 4))[0], 0)
        session.clock = 2201000
        session.invoke("worker", 0, 0)
        self.assertEqual(session.stops, [])

    def test_suspend_clears_watchdog_state(self):
        session = Session(self)
        session.invoke("worker", 0, 0)
        word(session.cpu, "recover_devices", 2)
        session.cpu.mem_write(SYMBOLS["stream_recoveries"] + 4, struct.pack("<I", 3))
        session.invoke("bridge_power_event", 0x400, 0, 0)
        for name in ("stream_started", "last_report", "recover_devices"):
            self.assertEqual(session.read_word(name), 0)
        self.assertEqual(struct.unpack("<I", session.cpu.mem_read(SYMBOLS["stream_recoveries"] + 4, 4))[0], 0)
        session.clock = 4001000
        session.invoke("worker", 0, 0)
        self.assertEqual(session.stops, [])

    def test_provider_unload_clears_sessions_and_replacement_is_rebound(self):
        session = Session(self)
        session.invoke("worker", 0, 0)
        word(session.cpu, "worker_started", 1)
        word(session.cpu, "active_source", 0)
        old_provider = session.provider
        session.provider = 0
        session.invoke("worker", 0, 0)
        for name in ("installed", "prepared_devices", "reads_ready", "wide_active",
                     "discovery_bypassed", "contexts", "queued", "serviced_devices"):
            self.assertEqual(session.read_word(name), 0, name)
        self.assertEqual(session.read_word("active_source"), 0xFFFFFFFF)
        session.provider = old_provider
        session.cpu.mem_write(session.provider + 44, struct.pack("<I", 8))
        rebound = []

        def bind(m, address, _, __):
            if address == SYMBOLS["bind_provider"]:
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), old_provider)
                self.assertEqual(session.read_word("installed"), 0)
                # The native-image binding boundary is mocked here; guards
                # and actual relocation are checked separately.
                rebound.append(1)
                word(m, "installed", 1)
                word(m, "provider_uid", 8)
                m.reg_write(reg.UC_MIPS_REG_V0, 0)
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        session.cpu.hook_add(UC_HOOK_CODE, bind)
        session.invoke("worker", 0, 0)
        self.assertEqual(rebound, [1])
        self.assertEqual(session.read_word("worker_started"), 1)
        self.assertEqual(session.read_word("provider_uid"), 8)

    def test_native_incoming_event_defers_queries_and_starts_without_xmb(self):
        session = Session(self)
        self.assertEqual(session.invoke("native_setup_checked", 1), 0)
        self.assertEqual(session.starts, [])
        self.assertEqual(session.reads, [])
        self.assertEqual(session.read_word("pending_devices"), 2)
        self.assertEqual(session.read_word("checked_devices"), 2)
        session.invoke("worker", 0, 0)
        self.assertEqual((len(session.starts), len(session.writes)), (1, 1))

    def test_boot_finds_an_already_connected_paired_controller(self):
        session = Session(self)
        session.invoke("worker", 0, 0)
        self.assertEqual((len(session.starts), len(session.writes)), (1, 1))
        self.assertEqual(session.read_word("serviced_devices"), 2)
        self.assertEqual(session.read_word("prepared_devices"), 2)

    def test_repeated_notifications_do_not_duplicate_context_or_initialization(self):
        session = Session(self)
        session.invoke("worker", 0, 0)
        session.invoke("native_setup_checked", 1)
        session.invoke("native_setup_event", 1)
        session.invoke("worker", 0, 0)
        self.assertEqual((len(session.starts), len(session.writes)), (1, 1))

    def test_disconnect_reconnect_reinitializes_using_existing_pairing(self):
        session = Session(self)
        session.invoke("worker", 0, 0)
        word(session.cpu, "active_source", 0)
        session.flags = 0
        session.invoke("poll_links")
        self.assertEqual(session.read_word("prepared_devices"), 0)
        self.assertEqual(session.read_word("reads_ready"), 0)
        self.assertEqual(session.read_word("active_source"), 0xFFFFFFFF)
        # Sony completes native teardown; the stored paired record is intact.
        session.cpu.mem_write(session.physical(session.context), b"\0" * 48)
        session.cpu.mem_write(session.physical(session.states + 2 * 12 + 0x7C), b"\0" * 12)
        session.flags, session.clock = 0x202, 500000
        session.invoke("worker", 0, 0)
        self.assertEqual((len(session.starts), len(session.writes)), (2, 2))
        self.assertEqual(session.read_word("prepared_devices"), 2)

    def test_resume_rediscovers_pairing_and_restores_input_setup(self):
        session = Session(self)
        session.invoke("worker", 0, 0)
        session.invoke("bridge_power_event", 0x400, 0, 0)
        session.cpu.mem_write(session.physical(session.context), b"\0" * 48)
        session.invoke("bridge_power_event", 0x400000, 0, 0)
        session.invoke("worker", 0, 0)
        self.assertEqual((len(session.starts), len(session.writes)), (2, 2))

    def test_new_generation_releases_previous_sink_before_accepting_input(self):
        session = Session(self)
        session.invoke("worker", 0, 0)
        word(session.cpu, "active_source", 0)
        word(session.cpu, "generations", 2)
        session.invoke("native_setup_event", 1)
        session.invoke("worker", 0, 0)
        self.assertEqual(session.read_word("active_source"), 0xFFFFFFFF)
        self.assertEqual(len(session.starts), 1)  # existing native context reused
        self.assertEqual(len(session.writes), 2)  # new session initialized

    def test_resume_preserves_wide_reads_when_native_context_survives(self):
        session = Session(self)
        session.invoke("worker", 0, 0)
        session.invoke("bridge_power_event", 0x400, 0, 0)
        session.invoke("bridge_power_event", 0x400000, 0, 0)
        session.invoke("worker", 0, 0)
        self.assertEqual(session.read_word("discovery_bypassed"), 1)
        self.assertEqual((len(session.starts), len(session.writes)), (1, 2))

    def test_metadata_not_ready_is_retried_without_remote_discovery(self):
        session = Session(self)
        session.query_error = 0x802F0203
        session.invoke("worker", 0, 0)
        self.assertEqual(session.starts, [])
        session.query_error, session.clock = 0, 500000
        session.invoke("worker", 0, 0)
        self.assertEqual((len(session.starts), len(session.writes)), (1, 1))

    def test_link_poll_does_not_start_other_bluetooth_profiles(self):
        session = Session(self)
        session.flags = 0x1002
        session.invoke("worker", 0, 0)
        self.assertEqual(session.starts, [])
        self.assertEqual(session.reads, [])

    def test_checked_native_setup_failure_retains_disconnect_cleanup(self):
        session = Session(self)
        session.matching = False
        session.setup_error = 0x802F0140
        session.invoke("native_setup_checked", 1)
        session.invoke("worker", 0, 0)
        self.assertEqual(session.closes, [1])
        self.assertEqual(session.writes, [])

    def test_dualsense_setup_failure_keeps_link_and_backs_off(self):
        session = Session(self)
        session.setup_error = 0x802F0140
        session.invoke("native_setup_checked", 1)
        session.invoke("worker", 0, 0)
        self.assertEqual(session.closes, [])
        self.assertEqual(session.flags, 0x202)
        session.clock = 300000
        session.invoke("worker", 0, 0)
        self.assertEqual(len(session.starts), 1)
        session.setup_error, session.clock = 0, 1100000
        session.invoke("worker", 0, 0)
        self.assertEqual((len(session.starts), len(session.writes)), (2, 1))
        self.assertEqual(session.closes, [])

    def seed_old_context(self, session, callback=0):
        session.cpu.mem_write(session.physical(session.context), struct.pack("<IHH", 2, 1, 0))
        # The bridge must not dereference this old, potentially freed entry.
        session.cpu.mem_write(session.physical(session.context + 40), struct.pack("<I", 0xDEADBEEF))
        session.cpu.mem_write(session.physical(session.states + 2 * 12 + 0x7C), struct.pack(
            "<III", callback, session.data, session.context))

    def test_new_prx_rebuilds_surviving_sony_context_without_repairing(self):
        session = Session(self)
        self.seed_old_context(session)
        session.invoke("worker", 0, 0)
        self.assertEqual((session.stops, session.starts), ([1], [1]))
        self.assertEqual(len(session.writes), 1)
        self.assertEqual(session.closes, [])

    def test_failed_owned_read_recovers_local_context_without_disconnect(self):
        session = Session(self)
        session.invoke("worker", 0, 0)
        word(session.cpu, "reads_ready", 0)
        word(session.cpu, "wide_active", 0)
        word(session.cpu, "serviced_devices", 0)
        word(session.cpu, "generations", 2)
        word(session.cpu, "active_source", 0)
        session.clock = 300000
        session.invoke("worker", 0, 0)
        self.assertEqual((len(session.stops), len(session.starts), len(session.writes)), (1, 2, 2))
        self.assertEqual(session.closes, [])
        self.assertEqual(session.read_word("active_source"), 0xFFFFFFFF)

    def test_foreign_channel_owner_prevents_context_reset(self):
        session = Session(self)
        self.seed_old_context(session, callback=0x0A700A00)
        session.invoke("worker", 0, 0)
        self.assertEqual((session.stops, session.starts, session.closes), ([], [], []))

    def test_failed_local_reset_does_not_disconnect_or_start_over_old_context(self):
        session = Session(self)
        self.seed_old_context(session)
        session.stop_error = 0x80300009
        session.invoke("worker", 0, 0)
        self.assertEqual((session.stops, session.starts, session.closes), ([1], [], []))
        session.clock = 300000
        session.invoke("worker", 0, 0)
        self.assertEqual(len(session.stops), 1)
        session.stop_error, session.clock = 0, 1100000
        session.invoke("worker", 0, 0)
        self.assertEqual((len(session.stops), len(session.starts)), (2, 1))

    def test_unchecked_native_setup_failure_does_not_add_disconnect(self):
        session = Session(self)
        session.setup_error = 0x802F0140
        session.invoke("native_setup_event", 1)
        session.invoke("worker", 0, 0)
        self.assertEqual(session.closes, [])


if __name__ == "__main__":
    unittest.main()
