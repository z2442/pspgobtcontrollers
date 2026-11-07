"""Run read-time input injection through the actual captured Sony driver."""
import struct
import unittest
import zlib
from unicorn import mips_const as reg, UC_HOOK_CODE

import test_controller_execution as controller
from test_bridge_execution import SYMBOLS, word


class ReadTimeInput(unittest.TestCase):
    def run_driver(self, check):
        for game in (False, True):
            with self.subTest(game=game):
                controller.ControllerSampler.exercise(self, game, check=check)

    def sender(self, cpu, invoke, clock):
        def send(face=0, shoulders=0, ps=1, axes=(128, 128, 128, 128)):
            clock[0] += 1000
            report = bytearray(78)
            report[0] = 0x31
            report[2:6] = bytes(axes)
            report[9:12] = bytes([face | 8, shoulders, ps])
            struct.pack_into("<I", report, 74, zlib.crc32(b"\xA1" + report[:74]))
            cpu.mem_write(0x0A600300, bytes(report))
            self.assertEqual(invoke(SYMBOLS["pspgobtSubmitReport"], [0x0A600300, 78]), 0)
        return send

    def test_press_and_release_reach_all_buffer_modes_without_another_native_sample(self):
        def check(cpu, invoke, base, state, clock, put, physical):
            send = self.sender(cpu, invoke, clock)
            self.assertEqual(invoke(SYMBOLS["gobt_ctrl_read_active"], []), 1)
            for flags in range(4):
                # Supply a real native sample first, then change remote input.
                invoke(base + 0x968, [0, 128, 128])
                ring = bytes(cpu.mem_read(physical(state + 0xF4), 4))
                send(face=0x40)  # Circle replaces the previously held Cross.
                self.assertEqual(invoke(base + 0x1E70, [0x0A600400, 1, 0, flags], 0x100000), 1)
                buttons = struct.unpack("<I", cpu.mem_read(0x0A600404, 4))[0]
                self.assertEqual((~buttons if flags & 1 else buttons) & 0xFFFF, 0x2000)
                self.assertEqual(bytes(cpu.mem_read(physical(state + 0xF4), 4)), ring)
                if flags & 2:
                    invoke(base + 0x968, [0, 128, 128])  # satisfy the next blocking native read
                send()
                self.assertEqual(invoke(base + 0x1E70, [0x0A600400, 1, 0, flags], 0x100000), 1)
                buttons = struct.unpack("<I", cpu.mem_read(0x0A600404, 4))[0]
                self.assertEqual((~buttons if flags & 1 else buttons) & 0xFFFF, 0)
        self.run_driver(check)

    def test_multiple_frames_extended_port_and_timestamps_are_preserved(self):
        def check(cpu, invoke, base, state, clock, put, physical):
            send = self.sender(cpu, invoke, clock)
            send(face=0x40, shoulders=1, axes=(10, 20, 30, 40))
            self.assertEqual(invoke(base + 0x1E70, [0x0A600400, 2, 0, 0], 0x100000), 2)
            for i in range(2):
                self.assertEqual(struct.unpack("<I", cpu.mem_read(0x0A600404 + i * 16, 4))[0] & 0xFFFF, 0x2100)
            for flags in (4, 5):
                self.assertEqual(invoke(base + 0x1E70, [0x0A600400, 1, 1, flags], 0x100000), 1)
                frame = bytes(cpu.mem_read(0x0A600400, 48))
                self.assertEqual(struct.unpack_from("<I", frame)[0], 1000)
                buttons = struct.unpack_from("<I", frame, 4)[0]
                self.assertEqual((~buttons if flags & 1 else buttons) & 0x1FFFF, 0x2400)
                self.assertEqual(frame[8:12], bytes([10, 20, 30, 40]))
                self.assertEqual(struct.unpack_from("<I", frame, 32)[0], 255)
        self.run_driver(check)

    def test_native_button_mapping_keeps_stick_clicks_start_select_and_both_trigger_pairs(self):
        def check(cpu, invoke, base, state, clock, put, physical):
            send = self.sender(cpu, invoke, clock)
            for shoulders, face, expected in ((3, 0, 0x300), (12, 0, 0x300), (0xF0, 0xF0, 0xF00F)):
                send(face=face, shoulders=shoulders)
                invoke(base + 0x1E70, [0x0A600400, 1, 0, 0], 0x100000)
                self.assertEqual(struct.unpack("<I", cpu.mem_read(0x0A600404, 4))[0] & 0xFFFF, expected)
        self.run_driver(check)

    def test_received_packet_reaches_game_read_without_worker_or_sampler_and_keeps_crc_checks(self):
        def check(cpu, invoke, base, state, clock, put, physical):
            for name, value in (("installed", 1), ("generations", 7), ("active_source", 0)):
                word(cpu, name, value)
            cpu.mem_write(SYMBOLS["device_models"] + 4, struct.pack("<I", 1))
            cpu.mem_write(SYMBOLS["sources"], struct.pack("<7I", 7, 1, 1, 1, 0, 1, 1))
            ring = bytes(cpu.mem_read(physical(state + 0xF4), 4))
            for face, corrupt, expected in ((0x40, False, 0x2000), (0, True, 0x2000), (0, False, 0)):
                clock[0] += 1000
                report = bytearray(78)
                report[0], report[2:6], report[9] = 0x31, bytes([128] * 4), 8 | face
                struct.pack_into("<I", report, 74, zlib.crc32(b"\xA1" + report[:74]))
                if corrupt: report[20] ^= 1
                cpu.mem_write(0x0A608000, b"\xA1" + report)
                cpu.reg_write(reg.UC_MIPS_REG_T0, 0)
                invoke(SYMBOLS["capture"], [0, 79, 0x0A608000, 1])
                self.assertEqual(struct.unpack("<I", cpu.mem_read(SYMBOLS["queued"], 4))[0], 1)
                self.assertEqual(invoke(base + 0x1E70, [0x0A600400, 1, 0, 0], 0x100000), 1)
                self.assertEqual(struct.unpack("<I", cpu.mem_read(0x0A600404, 4))[0] & 0xFFFF, expected)
                self.assertEqual(struct.unpack("<I", cpu.mem_read(SYMBOLS["queued"], 4))[0], 0)
                self.assertEqual(bytes(cpu.mem_read(physical(state + 0xF4), 4)), ring)
        self.run_driver(check)

    def test_physical_controls_sampling_mode_and_native_button_masks_are_preserved(self):
        def check(cpu, invoke, base, state, clock, put, physical):
            send = self.sender(cpu, invoke, clock)
            invoke(base + 0x968, [0x4000, 230, 200])  # physical Cross and stick
            send(face=0x40, axes=(192, 120, 128, 128))
            invoke(base + 0x1E70, [0x0A600400, 1, 0, 0], 0x100000)
            frame = bytes(cpu.mem_read(0x0A600400, 16))
            self.assertEqual(struct.unpack_from("<I", frame, 4)[0] & 0xFFFF, 0x6000)
            self.assertEqual(frame[8:10], bytes([230, 200]))
            send()
            invoke(base + 0x1E70, [0x0A600400, 1, 0, 0], 0x100000)
            self.assertEqual(struct.unpack("<I", cpu.mem_read(0x0A600404, 4))[0] & 0xFFFF, 0x4000)
            put(state + 0x298, 0x3FFFF & ~0x2000)
            send(face=0x40)
            invoke(base + 0x1E70, [0x0A600400, 1, 0, 0], 0x100000)
            self.assertEqual(struct.unpack("<I", cpu.mem_read(0x0A600404, 4))[0] & 0x2000, 0)
            invoke(base + 0x1E70, [0x0A600400, 1, 0, 0])
            self.assertEqual(struct.unpack("<I", cpu.mem_read(0x0A600404, 4))[0] & 0x2000, 0x2000)
            put(state + 0x29C, 0x3FFFF & ~0x2000)
            invoke(base + 0x1E70, [0x0A600400, 1, 0, 0])
            self.assertEqual(struct.unpack("<I", cpu.mem_read(0x0A600404, 4))[0] & 0x2000, 0)
            cpu.mem_write(physical(state + 12), bytes(2))
            invoke(base + 0x968, [0, 128, 128])
            send(axes=(255, 0, 128, 128))
            invoke(base + 0x1E70, [0x0A600400, 1, 0, 0], 0x100000)
            self.assertEqual(bytes(cpu.mem_read(0x0A600408, 2)), bytes([128, 128]))
        self.run_driver(check)

    def test_latch_edges_are_immediate_independent_for_user_and_kernel_and_clear_only_on_read(self):
        def check(cpu, invoke, base, state, clock, put, physical):
            send = self.sender(cpu, invoke, clock)
            dst = 0x0A600400
            invoke(base + 0x153C, [dst], 0x100000)  # consume initial user edges
            send(face=0x40)
            for _ in range(2):
                invoke(base + 0x1490, [dst], 0x100000)
                make, release, pressed, _ = struct.unpack("<4I", cpu.mem_read(dst, 16))
                self.assertEqual((make & 0xFFFF, release & 0xFFFF, pressed & 0xFFFF), (0x2000, 0x4110, 0x2000))
            invoke(base + 0x153C, [dst], 0x100000)
            invoke(base + 0x1490, [dst], 0x100000)
            self.assertEqual(struct.unpack("<2I", cpu.mem_read(dst, 8)), (0, 0))
            invoke(base + 0x1490, [dst])
            self.assertEqual(struct.unpack("<I", cpu.mem_read(dst, 4))[0] & 0xFFFF, 0x6110)
            send()
            invoke(base + 0x1490, [dst], 0x100000)
            self.assertEqual(struct.unpack("<I", cpu.mem_read(dst + 4, 4))[0] & 0xFFFF, 0x2000)
        self.run_driver(check)

    def test_disconnect_timeout_and_suspend_release_without_a_native_sample(self):
        def check(cpu, invoke, base, state, clock, put, physical):
            send = self.sender(cpu, invoke, clock)
            for action in ("disconnect", "timeout", "suspend"):
                send(face=0x40, axes=(255, 0, 128, 128))
                if action == "disconnect": invoke(SYMBOLS["pspgobtDisconnect"], [])
                elif action == "timeout": clock[0] += 250000
                else: invoke(SYMBOLS["power_event"], [0x400, 0, 0, 0])
                invoke(base + 0x1E70, [0x0A600400, 1, 0, 0], 0x100000)
                frame = bytes(cpu.mem_read(0x0A600400, 16))
                self.assertEqual(struct.unpack_from("<I", frame, 4)[0] & 0xFFFF, 0)
                self.assertEqual(frame[8:10], bytes([128, 128]))
        self.run_driver(check)

    def test_errors_installation_guards_and_foreign_hooks_remain_intact(self):
        def check(cpu, invoke, base, state, clock, put, physical):
            for count in (0, 65):
                cpu.mem_write(0x0A600400, bytes([0xCC]) * 48)
                self.assertEqual(invoke(base + 0x1E70, [0x0A600400, count, 0, 0], 0x100000), 0x80000104)
                self.assertEqual(bytes(cpu.mem_read(0x0A600400, 48)), bytes([0xCC]) * 48)
            handler, source = [struct.unpack("<I", cpu.mem_read(physical(state + offset), 4))[0]
                               for offset in (0x304, 0x30C)]
            invoke(SYMBOLS["gobt_ctrl_read_shutdown"], [base + 0x11F0])
            for offset in (0x1E70, 0x1490, 0x153C):
                original = bytes(cpu.mem_read(physical(base + offset), 4))
                put(base + offset, 0)
                self.assertEqual(invoke(SYMBOLS["gobt_ctrl_read_install"], [base + 0x11F0, handler, source]), 0)
                cpu.mem_write(physical(base + offset), original)
            self.assertEqual(invoke(SYMBOLS["gobt_ctrl_read_install"], [base + 0x11F0, handler, source]), 1)
            put(base + 0x1490, 0x03E00008)  # another plugin owns this entry now
            put(base + 0x1494, 0)
            self.assertEqual(invoke(SYMBOLS["gobt_ctrl_read_active"], []), 0)
            invoke(SYMBOLS["gobt_ctrl_read_shutdown"], [base + 0x11F0])
            self.assertEqual(struct.unpack("<2I", cpu.mem_read(physical(base + 0x1490), 8)), (0x03E00008, 0))
        self.run_driver(check)

    def test_active_play_keeps_power_timer_alive_from_worker_but_neutral_or_stale_input_does_not(self):
        def check(cpu, invoke, base, state, clock, put, physical):
            send = self.sender(cpu, invoke, clock)
            ticks = []
            def tick(m, address, size, user):
                if address == SYMBOLS["scePowerTick"]:
                    ticks.append(m.reg_read(reg.UC_MIPS_REG_A0))
            cpu.hook_add(UC_HOOK_CODE, tick)
            invoke(SYMBOLS["gobt_sink_maintain"], [])
            invoke(SYMBOLS["gobt_sink_maintain"], [])
            self.assertEqual(ticks, [0])
            clock[0] += 1000000
            send(face=0x20)
            invoke(SYMBOLS["gobt_sink_maintain"], [])
            self.assertEqual(ticks, [0, 0])
            clock[0] += 1000000
            invoke(SYMBOLS["gobt_sink_maintain"], [])
            self.assertEqual(ticks, [0, 0])  # expired reports cannot keep the PSP awake
            send(ps=0)
            invoke(SYMBOLS["gobt_sink_maintain"], [])
            self.assertEqual(ticks, [0, 0])
        self.run_driver(check)


if __name__ == "__main__":
    unittest.main()
