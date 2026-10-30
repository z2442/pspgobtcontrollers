"""Execute Sony's manager state updates and XMB's native connection query."""
import struct
import unittest

from unicorn import UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import SYMBOLS, word
from test_install_execution import Installation


class ManagerSession(Installation):
    def __init__(self, case, game=False, selected=True):
        super().__init__(case, game)
        case.assertEqual(self.call("gobt_install_input_bridge", self.module, 0x06060010), 0)
        self.state, self.event = self.bsbase + 0xDD00, 0x0A600400
        self.entry = self.state + 0xE4
        self.flags, self.notifications, self.disconnects = 0, [], []
        self.disconnect_result = 0
        self.mock_connect, self.connects = False, []
        self.mock_cleanup, self.cleanups = False, []
        self.write(self.entry, struct.pack("<HHI", 1, 0x202, 11))
        self.write(self.state + 0xA4, struct.pack("<H", 2))
        self.write(self.state + 0xA8, struct.pack("<I", 12))
        self.write(self.state + 0x118, struct.pack("<I", 13))
        word(self.cpu, "prepared_devices", 2 if selected else 0)
        # The capture executes in KSEG0; the test ELF itself is linked at zero.
        # Point the new native call at its compiled adapter and normalize that
        # ELF alias on entry, leaving captured firmware/import addresses intact.
        self.write(self.bsbase + 0x2880, struct.pack("<I",
                   0x0C000000 | ((SYMBOLS["connect_poll_stub"] >> 2) & 0x03FFFFFF)))
        self.cpu.hook_add(UC_HOOK_CODE, self.firmware)

    def write(self, pointer, data):
        self.cpu.mem_write(self.physical(pointer), data)

    def firmware(self, m, address, _, __):
        if 0x80000000 <= address < 0x80010000:
            m.reg_write(reg.UC_MIPS_REG_PC, address & 0x1FFFFFFF)
            return
        a, b, _, d = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                                            reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3)]
        returns = {self.bsbase + 0xCA78: 1, self.bsbase + 0xCA80: 0,
                   self.bsbase + 0x71DC: 0, self.bsbase + 0xCC60: 0}
        if address == self.bsbase + 0xCAB8:
            self.case.assertEqual(a, 11)
            self.write(d, struct.pack("<I", self.flags))
            # OR matching, no clear bits: Sony polls mask 2 on phase 3.
            self.case.assertEqual(m.reg_read(reg.UC_MIPS_REG_A2), 1)
            returns[address] = 0 if self.flags & b else 0x800201AF
        if address == self.bsbase + 0xCB20:
            self.case.assertEqual(a, 11)
            self.flags &= b
            returns[address] = 0
        if address == self.bsbase + 0xCAA0:
            self.case.assertEqual(a, 11)
            self.flags |= b
            returns[address] = 0
        if address == self.bsbase + 0xCB70:
            self.notifications.append((a, b))
            returns[address] = 0
        if address == self.bsbase + 0x2B10:
            self.case.assertEqual((a, b, m.reg_read(reg.UC_MIPS_REG_A2)), (1, 2, 0))
            self.disconnects.append(a)
            returns[address] = self.disconnect_result
        if address == self.bsbase + 0x2044 and self.mock_connect:
            self.case.assertEqual(struct.unpack("<H", m.mem_read(self.physical(a + 2), 2))[0], 0)
            self.connects.append(a)
            returns[address] = 0
        if address == self.bsbase + 0xA2DC and self.mock_cleanup:
            self.case.assertEqual(a, 1)
            self.cleanups.append(a)
            self.write(self.bsbase + 0xDE78, bytes(8))
            returns[address] = 0
        if address in returns:
            m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
            m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

    def run(self, address, argument):
        self.cpu.reg_write(reg.UC_MIPS_REG_A0, argument)
        self.cpu.reg_write(reg.UC_MIPS_REG_SP, 0x0A60F000)
        self.cpu.reg_write(reg.UC_MIPS_REG_RA, 0x0A700F00)
        self.cpu.emu_start(address, 0x0A700F00, count=200000)
        return self.cpu.reg_read(reg.UC_MIPS_REG_V0)

    def indication(self, phase, patched=True, profile=0x202):
        self.write(self.event, struct.pack("<HHH", 1, phase, profile))
        return self.run(SYMBOLS["native_session_event"] if patched else self.bsbase + 0x2044, self.event)

    def connection(self):
        # Actual CCED8471 export used by the XMB Connect/Disconnect menu.
        return self.run(self.bsbase + 0x5390, 1)


class NativeManager(unittest.TestCase):
    def test_reproduce_sony_terminal_event_leaving_connected_without_handshake_bit(self):
        for game in (False, True):
            s = ManagerSession(self, game)
            s.flags = 2  # completed outgoing operation, not an empty flag
            self.assertEqual(s.connection(), 0x202)
            self.assertEqual(s.indication(3, patched=False), 0)
            self.assertEqual(s.connection(), 0x202)
            self.assertEqual(s.flags, 6)
            self.assertEqual(s.notifications, [])

    def test_real_terminal_event_clears_native_xmb_status_and_notifies_observers(self):
        for game in (False, True):
            s = ManagerSession(self, game)
            self.assertEqual(s.indication(3), 0)
            self.assertEqual(s.connection(), 0)
            self.assertEqual(bytes(s.cpu.mem_read(s.physical(s.entry), 4)), bytes(4))
            self.assertEqual(s.notifications, [(12, 0x20202), (13, 0x1000202)])
            self.assertEqual(s.read_word("pending_closures"), 2)
            self.assertEqual(s.disconnects, [])

    def test_phase_two_only_dispatches_native_failure_signal_without_latching_close(self):
        for game in (False, True):
            s = ManagerSession(self, game)
            self.assertEqual(s.indication(2), 0)
            self.assertEqual(s.flags, 4)  # wakes Connect's native 1|4 wait
            self.assertEqual(s.read_word("disconnect_requests"), 0)
            self.assertEqual(s.read_word("closing_devices"), 0)
            self.assertEqual(s.call("gobt_input_bridge_connect_ready", 1), 0)
            self.assertEqual(s.disconnects, [])
            self.assertEqual(s.connection(), 0x202)

    def test_original_phase_three_finalizes_when_poll_mask_does_not_match(self):
        for flags in (0, 1, 4, 8):
            s = ManagerSession(self)
            s.flags = flags
            self.assertEqual(s.indication(3, patched=False), 0)
            self.assertEqual(s.connection(), 0)

    def test_actual_outgoing_connect_consumes_phase_two_as_failure_and_can_retry(self):
        for game in (False, True):
            s = ManagerSession(self, game)
            s.write(s.state + 0xA4, bytes(64))
            s.write(s.entry, bytes(48))
            s.write(s.entry + 4, struct.pack("<I", 11))
            s.write(s.state + 0x152, b"\x02")
            s.flags = 2
            waiting, commands, deleted = [], [], []

            def transport(m, address, _, __):
                a, b, c, d = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                                                      reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3)]
                if address == s.bsbase + 0x3720:
                    self.assertEqual(struct.unpack("<H", m.mem_read(a, 2))[0], 0x68)
                    commands.append(0x68)
                    m.mem_write(b + 4, struct.pack("<H", 1))  # pending connection
                elif address == s.bsbase + 0xCAD0:
                    self.assertEqual((a, b, c), (11, 5, 1))
                    waiting.append(d)
                    m.emu_stop()
                    return
                elif address == s.bsbase + 0xCBC0:
                    deleted.append(a)
                else:
                    return
                m.reg_write(reg.UC_MIPS_REG_V0, 0)
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

            s.cpu.hook_add(UC_HOOK_CODE, transport)
            s.cpu.reg_write(reg.UC_MIPS_REG_A1, 0)
            s.run(s.bsbase + 0xA730, 1)  # actual F188A405 and Connect helper
            self.assertEqual((commands, len(waiting)), ([0x68], 1))
            saved = s.cpu.context_save()
            stack = bytes(s.cpu.mem_read(0x0A60E000, 0x1000))
            self.assertEqual(s.indication(2), 0)
            self.assertEqual(s.flags & 5, 4)
            s.cpu.context_restore(saved)
            s.cpu.mem_write(0x0A60E000, stack)
            s.cpu.mem_write(waiting[0], struct.pack("<I", s.flags))
            s.cpu.reg_write(reg.UC_MIPS_REG_V0, 0)
            s.cpu.emu_start(s.cpu.reg_read(reg.UC_MIPS_REG_RA), 0x0A700F00, count=200000)
            self.assertEqual(s.cpu.reg_read(reg.UC_MIPS_REG_V0), 0x802F0140)
            self.assertEqual(deleted, [11])
            self.assertEqual(s.connection(), 0)
            self.assertEqual(s.read_word("disconnect_requests"), 0)
            self.assertEqual(s.call("gobt_input_bridge_connect_ready", 1), 0)
            self.assertEqual(s.disconnects, [])

    def test_phase_three_finishes_for_all_native_handshake_states(self):
        for flags in (0, 1, 2, 4, 8, 10):
            s = ManagerSession(self)
            s.flags = flags
            self.assertEqual(s.indication(3), 0)
            self.assertEqual(s.connection(), 0)
            self.assertEqual(s.read_word("pending_closures"), 2)
            self.assertEqual(s.disconnects, [])

    def test_phase_two_does_not_duplicate_or_release_a_settings_disconnect(self):
        s = ManagerSession(self)
        s.call("gobt_input_bridge_disconnecting", 1)
        s.indication(2)
        self.assertEqual(s.read_word("disconnect_requests"), 2)
        self.assertEqual(s.disconnects, [])
        s.indication(3)
        self.assertEqual(s.connection(), 0)

    def test_new_connection_supersedes_settings_disconnect_and_retires_old_read(self):
        s = ManagerSession(self)
        context = s.bsbase + 0xDE78
        s.write(context, struct.pack("<IHH", 2, 1, 1))
        word(s.cpu, "discovery_bypassed", 1)
        word(s.cpu, "wide_active", 1)
        word(s.cpu, "wide_native", 0x88246000)
        s.call("gobt_input_bridge_disconnecting", 1)
        s.mock_connect = True
        self.assertEqual(s.indication(0), 0)
        self.assertEqual(s.disconnects, [])
        self.assertEqual(len(s.connects), 1)
        self.assertEqual(s.read_word("pending_closures"), 2)
        s.mock_cleanup = True
        s.call("finish_closures")
        self.assertEqual(s.cleanups, [1])
        self.assertEqual(s.read_word("closing_devices"), 0)
        self.assertEqual(s.read_word("wide_active"), 0)
        self.assertEqual(s.connection(), 0x202)  # preserve the new link

    def test_unknown_controller_and_other_profiles_keep_original_event_dispatch(self):
        for selected, profile in ((False, 0x202), (True, 0x204)):
            s = ManagerSession(self, selected=selected)
            s.flags = 2
            s.indication(3, profile=profile)
            self.assertEqual(s.connection(), 0x202)
            self.assertEqual(s.read_word("pending_closures"), 0)

    def test_unmatched_or_already_retired_entry_is_not_finalized_again(self):
        for entry in (bytes(8), struct.pack("<HHI", 2, 0x202, 11)):
            s = ManagerSession(self)
            s.write(s.entry, entry)
            s.indication(3)
            self.assertEqual(s.notifications, [])
            self.assertEqual(s.read_word("pending_closures"), 0)

    def test_event_guard_refuses_changed_instructions_in_xmb_and_game(self):
        offsets = (0xA24, 0xA28, 0x2044, 0x204C, 0x2050, 0x2058, 0x2070, 0x2074, 0x2078,
                   0x2158, 0x216C, 0x2190, 0x21A4, 0x21B8, 0x21C8,
                   0x21CC, 0x21E0, 0x21E4, 0x2214, 0x2224, 0x2230,
                   0x255C, 0x2560, 0x2564, 0x256C, 0x25B8, 0x25C8, 0x2628, 0x262C)
        for game in (False, True):
            for offset in offsets:
                s = Installation(self, game)
                site = s.physical(s.bsbase + offset)
                value = bytes(s.cpu.mem_read(site, 4))
                s.cpu.mem_write(site, bytes([value[0] ^ 1]) + value[1:])
                self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0xFFFFFFFE)


if __name__ == "__main__":
    unittest.main()
