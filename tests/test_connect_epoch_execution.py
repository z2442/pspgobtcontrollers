"""Execute Sony Connect with reused event flags and fresh completion signals."""
import struct
import unittest
from unicorn import UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import SYMBOLS, word
from test_manager_session_execution import ManagerSession


class ConnectAttempt(ManagerSession):
    def __init__(self, case, game=False, selected=True, flags=6, patched=True):
        super().__init__(case, game, selected)
        self.write(self.state + 0xA4, bytes(64))
        self.write(self.entry, bytes(48))
        self.write(self.entry + 4, struct.pack("<I", 11))
        self.write(self.state + 0x152, b"\x02")
        self.flags = flags
        self.commands, self.deleted, self.waits = [], [], []
        self.waiting = False
        # Captured kernel executes high; test ELF is linked at zero. Adapt
        # this one JAL to its real compiled stub and normalize its KSEG alias.
        target = SYMBOLS["connect_poll_stub"] if patched else self.bsbase + 0xCAB8
        self.write(self.bsbase + 0x2880, struct.pack("<I", 0x0C000000 | ((target >> 2) & 0x03FFFFFF)))
        self.cpu.hook_add(UC_HOOK_CODE, self.transport)

    def transport(self, m, address, _, __):
        if 0x80000000 <= address < 0x80010000:
            m.reg_write(reg.UC_MIPS_REG_PC, address & 0x1FFFFFFF)
            return
        a, b, c, d = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                                            reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3)]
        if address == self.bsbase + 0x3720:
            self.case.assertEqual(struct.unpack("<H", m.mem_read(a, 2))[0], 0x68)
            self.commands.append(0x68)
            m.mem_write(b + 4, struct.pack("<H", 1))  # real command pending
        elif address == self.bsbase + 0xCAD0:
            self.case.assertEqual((a, b, c), (11, 5, 1))
            self.waits.append((d, self.flags))
            if not self.flags & b:
                self.waiting = True
                m.emu_stop()
                return
            m.mem_write(d, struct.pack("<I", self.flags))
        elif address == self.bsbase + 0xCBC0:
            self.deleted.append(a)
        else:
            return
        m.reg_write(reg.UC_MIPS_REG_V0, 0)
        m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

    def connect(self):
        self.cpu.reg_write(reg.UC_MIPS_REG_A1, 0)
        return self.run(self.bsbase + 0xA730, 1)

    def complete(self, success):
        self.case.assertTrue(self.waiting)
        self.flags |= 1 if success else 4
        self.write(self.waits[-1][0], struct.pack("<I", self.flags))
        self.cpu.reg_write(reg.UC_MIPS_REG_V0, 0)
        self.cpu.emu_start(self.cpu.reg_read(reg.UC_MIPS_REG_RA), 0x0A700F00, count=200000)
        return self.cpu.reg_read(reg.UC_MIPS_REG_V0)


class ConnectEpoch(unittest.TestCase):
    def test_reproduce_false_0140_before_new_radio_operation_completes(self):
        for game in (False, True):
            s = ConnectAttempt(self, game, patched=False)
            self.assertEqual(s.connect(), 0x802F0140)
            self.assertEqual(s.commands, [0x68])  # request really sent despite immediate error
            self.assertFalse(s.waiting)  # stale bit 4 satisfied WaitEventFlag
            self.assertEqual(s.deleted, [11])

    def test_reused_closed_entry_waits_for_fresh_success_instead_of_old_failure(self):
        for game in (False, True):
            s = ConnectAttempt(self, game)
            s.connect()
            self.assertTrue(s.waiting)
            self.assertEqual(s.flags, 2)
            self.assertEqual(s.commands, [0x68])
            self.assertEqual(s.deleted, [])
            self.assertEqual(s.complete(True), 0)

    def test_fresh_failure_still_returns_original_0140(self):
        s = ConnectAttempt(self)
        s.connect()
        self.assertTrue(s.waiting)
        self.assertEqual(s.complete(False), 0x802F0140)
        self.assertEqual(s.deleted, [11])

    def test_other_controllers_keep_original_flags_and_result(self):
        s = ConnectAttempt(self, selected=False)
        self.assertEqual(s.connect(), 0x802F0140)
        self.assertFalse(s.waiting)

    def test_incoming_busy_bit_keeps_original_busy_error_without_sending_command(self):
        for flags in (10, 14, 15):
            s = ConnectAttempt(self, flags=flags)
            self.assertEqual(s.connect(), 0x802F0144)
            self.assertEqual(s.commands, [])
            self.assertEqual(s.flags, flags)

    def test_old_success_also_cannot_satisfy_a_fresh_wait(self):
        for flags in (3, 7, 0x26):
            s = ConnectAttempt(self, flags=flags)
            s.connect()
            self.assertTrue(s.waiting)
            self.assertEqual(s.flags, flags & ~5)
            self.assertEqual(s.complete(True), 0)

    def test_actual_close_finalizer_leaves_old_failure_bit_and_next_connect_clears_it(self):
        s = ConnectAttempt(self, flags=2)
        s.write(s.entry, struct.pack("<HHI", 1, 0x202, 11))
        s.write(s.state + 0xA4, struct.pack("<H", 2))
        s.indication(2)
        s.indication(3)
        self.assertEqual(s.flags, 6)
        self.assertEqual(s.connection(), 0)
        s.mock_cleanup = True
        s.call("finish_closures")
        word(s.cpu, "prepared_devices", 2)  # XMB reclassifies the saved DualSense before Connect
        s.connect()
        self.assertTrue(s.waiting)
        self.assertEqual(s.flags, 2)
        self.assertEqual(s.complete(True), 0)

    def test_poll_guard_preserves_connected_other_profile_or_unfinished_operation(self):
        for case in ("connected", "profile", "uid", "entry", "quiescing", "no-close-bit"):
            s = ConnectAttempt(self, flags=4 if case == "no-close-bit" else 6)
            s.write(s.entry, struct.pack("<HHI", 1, 0x204 if case == "profile" else 0x202,
                                         12 if case == "uid" else 11))
            if case == "connected": s.write(s.state + 0xA4, struct.pack("<H", 2))
            if case == "quiescing": word(s.cpu, "quiescing", 1)
            before = s.flags
            for r, value in ((reg.UC_MIPS_REG_A1, 0xFFFFFFFF), (reg.UC_MIPS_REG_A2, 1),
                              (reg.UC_MIPS_REG_A3, 0x0A600600),
                              (reg.UC_MIPS_REG_T0, s.entry - 8 if case == "entry" else s.entry),
                              (reg.UC_MIPS_REG_T1, 1)):
                s.cpu.reg_write(r, value)
            self.assertEqual(s.run(SYMBOLS["native_connect_poll"], 11), 0)
            self.assertEqual(s.flags, before)

    def test_connect_guard_rejects_changed_native_instructions(self):
        from test_install_execution import Installation
        offsets = (0x2728, 0x2738, 0x2828, 0x2854, 0x285C, 0x2874, 0x2878, 0x287C,
                   0x2880, 0x2884, 0x2888, 0x2894, 0x2898, 0x29EC, 0x29F4, 0x29F8,
                   0x2A0C, 0x2A10, 0x2A20, 0x2A2C)
        for game in (False, True):
            for offset in offsets:
                s = Installation(self, game)
                pointer = s.physical(s.bsbase + offset)
                original = bytes(s.cpu.mem_read(pointer, 4))
                s.cpu.mem_write(pointer, bytes([original[0] ^ 1]) + original[1:])
                self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0xFFFFFFFE)

    def test_adapter_is_published_before_live_hook_and_foreign_hook_is_preserved_on_exit(self):
        from test_install_execution import Installation
        s = Installation(self)
        site = s.physical(s.bsbase + 0x2880)
        original = bytes(s.cpu.mem_read(site, 4))
        snapshots = []
        def caches(m, address, _, __):
            if address == SYMBOLS["sceKernelIcacheInvalidateAll"]:
                snapshots.append((bytes(m.mem_read(site, 4)), bytes(m.mem_read(SYMBOLS["connect_poll_stub"], 16))))
        s.cpu.hook_add(UC_HOOK_CODE, caches)
        self.assertEqual(s.call("gobt_install_input_bridge", s.module, 0x06060010), 0)
        self.assertEqual(snapshots[0][0], original)
        self.assertEqual(struct.unpack("<II", snapshots[0][1][:8]), (0x02004021, 0x02604821))
        self.assertNotEqual(snapshots[-1][0], original)
        foreign = struct.pack("<I", 0x0C102030)
        s.cpu.mem_write(site, foreign)
        s.call("gobt_shutdown_input_bridge", 0)
        s.call("worker", 0, 0)
        self.assertEqual(bytes(s.cpu.mem_read(site, 4)), foreign)


if __name__ == "__main__":
    unittest.main()
