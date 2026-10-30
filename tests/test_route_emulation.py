"""Exercise generated MIPS route wrappers in Unicorn (optional test-mips target)."""
import ctypes
from pathlib import Path
import struct
import unittest

from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_LITTLE_ENDIAN, UC_HOOK_CODE
from unicorn import mips_const as reg

LIB = ctypes.CDLL(str(Path("build/libhidroute_host.so").resolve()))
LIB.gobt_build_route_code.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
LIB.gobt_build_route_code.restype = ctypes.c_int
LIB.gobt_route_hid_record.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
LIB.gobt_route_hid_record.restype = ctypes.c_int
LIB.gobt_pair_scan_record.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
LIB.gobt_pair_scan_record.restype = ctypes.c_int
LIB.gobt_hid_route_supported.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
LIB.gobt_hid_route_supported.restype = ctypes.c_int
LIB.gobt_build_hid_label_code.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32, ctypes.c_uint32]
LIB.gobt_build_hid_label_code.restype = ctypes.c_int


class RouteExecution(unittest.TestCase):
    def exercise(self, name, capability, kind, expected_kind):
        call, wrapper, syscall, native = 0x0A001000, 0x0A002000, 0x0A003000, 0x0A004000
        data, stack = 0x09E00000, 0x09F00F00
        cpu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN)
        cpu.mem_map(0x0A000000, 0x10000)
        cpu.mem_map(data, 0x1000)
        cpu.mem_map(0x09F00000, 0x1000)
        factory = 0x0A005000
        words = (ctypes.c_uint32 * 21)()
        self.assertEqual(LIB.gobt_build_route_code(words, wrapper, syscall, native, factory), 0)
        cpu.mem_write(wrapper, bytes(words))
        # Preserve the real call site's delay slot: move a1,sp (info record).
        cpu.mem_write(call, struct.pack("<II", 0x0C000000 | (wrapper >> 2), 0x03A02821))
        cpu.mem_write(syscall, struct.pack("<II", 0x03E00008, 0))
        # The native constructor returns its own result in the jr delay slot.
        cpu.mem_write(factory, struct.pack("<II", 0x03E00008, 0))
        cpu.mem_write(native, struct.pack("<II", 0x03E00008, 0x24021234))
        profile = bytearray(b"\xa5" * 44)
        struct.pack_into("<I", profile, 4, kind)
        info = bytearray(80)
        struct.pack_into("<I", info, 0, 80)
        encoded = name.encode("utf-16-le")
        info[6:6 + len(encoded)] = encoded
        struct.pack_into("<H", info, 72, capability)
        cpu.mem_write(data, bytes(profile))
        cpu.mem_write(stack, bytes(info))
        cpu.reg_write(reg.UC_MIPS_REG_SP, stack)
        cpu.reg_write(reg.UC_MIPS_REG_A0, data)
        cpu.reg_write(reg.UC_MIPS_REG_A1, 0xBADCAFE)
        cpu.reg_write(reg.UC_MIPS_REG_S0, 0x13572468)
        seen = []
        selected_route = name.startswith("DualSense ") and capability == 0x402 and kind <= 9

        def observe(machine, address, size, _):
            if address == syscall:
                self.assertEqual(machine.reg_read(reg.UC_MIPS_REG_A0), data)
                self.assertEqual(machine.reg_read(reg.UC_MIPS_REG_A1), stack)
                p = ctypes.create_string_buffer(bytes(machine.mem_read(data, 44)))
                i = ctypes.create_string_buffer(bytes(machine.mem_read(stack, 80)))
                selected = LIB.gobt_route_hid_record(p, 44, i, 80)
                machine.mem_write(data, p.raw[:44])
                # Model a syscall that clobbers caller-saved argument registers.
                for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                          reg.UC_MIPS_REG_V0, reg.UC_MIPS_REG_T0):
                    machine.reg_write(r, 0xBADCAFE)
                machine.reg_write(reg.UC_MIPS_REG_V0, selected)
            elif address == factory:
                seen.append(address)
                self.assertEqual(machine.reg_read(reg.UC_MIPS_REG_A0), data)
                self.assertEqual(machine.reg_read(reg.UC_MIPS_REG_A1), 2)
                self.assertEqual(struct.unpack("<I", machine.mem_read(data + 4, 4))[0], 9)
            elif address == native:
                seen.append(address)
                self.assertEqual(machine.reg_read(reg.UC_MIPS_REG_A0), data)
                self.assertEqual(machine.reg_read(reg.UC_MIPS_REG_A1), stack)
                self.assertEqual(machine.reg_read(reg.UC_MIPS_REG_RA), call + 8)
                self.assertEqual(machine.reg_read(reg.UC_MIPS_REG_SP), stack)

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.emu_start(call, call + 8, count=64)
        self.assertEqual(seen, [factory] if selected_route else [native])
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), 9 if selected_route else 0x1234)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), stack)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_S0), 0x13572468)
        expected = profile.copy()
        struct.pack_into("<I", expected, 4, expected_kind)
        self.assertEqual(bytes(cpu.mem_read(data, 44)), bytes(expected))
        self.assertEqual(bytes(cpu.mem_read(stack, 80)), bytes(info))

    def test_matching_dualsense(self):
        self.exercise("DualSense Wireless Controller", 0x0402, 2, 9)

    def test_other_device_unchanged(self):
        self.exercise("Other Controller", 0x0402, 2, 2)

    def test_other_capabilities_unchanged(self):
        self.exercise("DualSense Wireless Controller", 0x0202, 2, 2)

    def test_existing_ps3_profile_is_migrated_to_generic_hid(self):
        self.exercise("DualSense Wireless Controller", 0x0402, 1, 9)

    def test_existing_generic_hid_still_gets_methods(self):
        self.exercise("DualSense Wireless Controller", 0x0402, 2, 9)

    def test_other_ps3_controller_unchanged(self):
        self.exercise("PLAYSTATION(R)3 Controller", 0x0402, 1, 1)


class CapturedGenericHid(unittest.TestCase):
    def captures(self):
        for uid, base in (("04FE4C4D", 0x0A572000), ("050B8563", 0x0A572100)):
            path = Path(f"pspgobt/bluetooth_plugin_module-{uid}.text.bin")
            if not path.exists():
                self.skipTest("requires local Sony XMB capture")
            yield base, path.read_bytes()

    def test_generic_type_gets_actual_native_hid_methods_without_ps3_classification(self):
        for base, capture in self.captures():
            data, stack, wrapper, syscall, stop = 0x09E00000, 0x09F00F00, 0x0A560000, 0x0A561000, 0x0A562000
            cpu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN)
            cpu.mem_map(0x0A560000, 0x50000)
            cpu.mem_map(data, 0x1000)
            cpu.mem_map(0x09F00000, 0x1000)
            cpu.mem_write(base, capture)
            label = (ctypes.c_uint32 * 12)()
            self.assertEqual(LIB.gobt_build_hid_label_code(label, wrapper + 0x200, base), 0)
            cpu.mem_write(wrapper + 0x200, bytes(label))
            cpu.mem_write(base + 0x146C4, struct.pack("<II", 0x08000000 | ((wrapper + 0x200) >> 2), 0))
            words = (ctypes.c_uint32 * 21)()
            self.assertEqual(LIB.gobt_build_route_code(words, wrapper, syscall, base + 0x14E0C, base + 0x14F64), 0)
            cpu.mem_write(wrapper, bytes(words))
            cpu.mem_write(syscall, struct.pack("<II", 0x03E00008, 0))
            profile, info = bytearray(44), bytearray(80)
            struct.pack_into("<II", profile, 0, 3, 2)
            struct.pack_into("<I", info, 0, 80)
            name = "DualSense Wireless Controller".encode("utf-16le")
            info[6:6 + len(name)] = name
            struct.pack_into("<H", info, 72, 0x402)
            cpu.mem_write(data, bytes(profile))
            cpu.mem_write(stack, bytes(info))
            cpu.reg_write(reg.UC_MIPS_REG_SP, stack)
            cpu.reg_write(reg.UC_MIPS_REG_A0, data)
            cpu.reg_write(reg.UC_MIPS_REG_A1, stack)
            cpu.reg_write(reg.UC_MIPS_REG_RA, stop)
            appended, labels, operations = [], [], []

            def observe(m, address, _, __):
                if address == syscall:
                    p = ctypes.create_string_buffer(bytes(m.mem_read(data, 44)))
                    i = ctypes.create_string_buffer(bytes(m.mem_read(stack, 80)))
                    selected = LIB.gobt_route_hid_record(p, 44, i, 80)
                    m.mem_write(data, p.raw[:44])
                    m.reg_write(reg.UC_MIPS_REG_V0, selected)
                elif address == base + 0x1ABE0:
                    self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), 4)
                    m.reg_write(reg.UC_MIPS_REG_V0, data + 0x200)
                    m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))
                elif address == base + 0x1A734:
                    self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), data + 0x1C)
                    item = struct.unpack("<I", m.mem_read(m.reg_read(reg.UC_MIPS_REG_A1), 4))[0]
                    appended.append(item)
                    m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))
                elif address == base + 0x14E0C:
                    self.fail("selected HID must not re-enter the PS3-classifying constructor")
                elif address == base + 0x15F00:
                    key = bytes(m.mem_read(m.reg_read(reg.UC_MIPS_REG_A1), 32)).split(b"\0", 1)[0]
                    labels.append(key)
                    m.reg_write(reg.UC_MIPS_REG_V0, 0)
                    m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))
                elif address in (base + 0x1AAC8, base + 0x1AA70):
                    self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), 3)
                    if address == base + 0x1AAC8:
                        self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A1), 0)  # generic HID mode
                    operations.append(address)
                    m.reg_write(reg.UC_MIPS_REG_V0, 0x802F0140)
                    m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

            cpu.hook_add(UC_HOOK_CODE, observe)
            cpu.emu_start(wrapper, stop, count=300)
            self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), 9)
            self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), stack)
            self.assertEqual(appended, [data + 0x200])
            table = struct.unpack("<I", cpu.mem_read(data + 0x200, 4))[0]
            self.assertEqual(table, base + 0x20C28)
            self.assertEqual(struct.unpack("<II", cpu.mem_read(table + 8, 8)), (base + 0x15514, base + 0x15534))
            self.assertEqual(struct.unpack("<II", cpu.mem_read(data, 8)), (3, 9))
            self.assertEqual(bytes(cpu.mem_read(stack, 80)), bytes(info))
            # Execute the real type-label dispatch and both actual HID methods.
            cpu.reg_write(reg.UC_MIPS_REG_A0, data)
            cpu.reg_write(reg.UC_MIPS_REG_A1, data + 0x300)
            cpu.reg_write(reg.UC_MIPS_REG_RA, stop)
            cpu.emu_start(base + 0x146C4, stop, count=100)
            self.assertEqual(labels, [b"msg_bt_hid"])
            for offset in (0x15514, 0x15534):
                cpu.reg_write(reg.UC_MIPS_REG_A0, data + 0x200)
                cpu.reg_write(reg.UC_MIPS_REG_A1, 3)
                cpu.reg_write(reg.UC_MIPS_REG_RA, stop)
                cpu.emu_start(base + offset, stop, count=100)
                self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), 0x802F0140)
                self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), stack)
            self.assertEqual(operations, [base + 0x1AAC8, base + 0x1AA70])

    def test_factory_and_method_guards_accept_captures_and_refuse_changes(self):
        offsets = (0x14F64, 0x14F7C, 0x14F84, 0x14FD0, 0x14FD8, 0x14FDC,
                   0x14FE0, 0x14FEC, 0x15514, 0x15524, 0x15534, 0x1CA54,
                   0x20C28, 0x20C30, 0x20C34)
        for base, capture in self.captures():
            for firmware in (0x06060010, 0x06060110):
                original = ctypes.create_string_buffer(capture)
                self.assertEqual(LIB.gobt_hid_route_supported(firmware, original, len(capture), base), 1)
                for offset in offsets:
                    changed = bytearray(capture)
                    changed[offset] ^= 1
                    image = ctypes.create_string_buffer(bytes(changed))
                    self.assertEqual(LIB.gobt_hid_route_supported(firmware, image, len(changed), base), 0)


class PairCallExecution(unittest.TestCase):
    def exercise(self, name, matches, result):
        method, syscall, stop = 0x0A005000, 0x0A006000, 0x0A007000
        data, stack = 0x09E00000, 0x09F00F00
        cpu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN)
        cpu.mem_map(0x0A000000, 0x10000)
        cpu.mem_map(data, 0x1000)
        cpu.mem_map(0x09F00000, 0x1000)
        # The captured fallback method, with only its jal target replaced.
        cpu.mem_write(method, struct.pack("<7I", 0x27BDFFF0, 0xAFBF0000,
                      0x0C000000 | (syscall >> 2), 0x2485004F,
                      0x8FBF0000, 0x03E00008, 0x27BD0010))
        cpu.mem_write(syscall, struct.pack("<II", 0x03E00008, 0))
        obj = bytearray(88)
        struct.pack_into("<II", obj, 0, 0x0A009000, 84)
        encoded = name.encode("utf-16-le")
        obj[10:10 + len(encoded)] = encoded
        cpu.mem_write(data, bytes(obj))
        cpu.reg_write(reg.UC_MIPS_REG_A0, data)
        cpu.reg_write(reg.UC_MIPS_REG_A1, 0xBADCAFE)
        cpu.reg_write(reg.UC_MIPS_REG_SP, stack)
        cpu.reg_write(reg.UC_MIPS_REG_RA, stop)
        cpu.reg_write(reg.UC_MIPS_REG_S0, 0x13572468)
        seen = []

        def observe(machine, address, size, _):
            if address == syscall:
                seen.append(address)
                self.assertEqual(machine.reg_read(reg.UC_MIPS_REG_A0), data)
                self.assertEqual(machine.reg_read(reg.UC_MIPS_REG_A1), data + 79)
                record = ctypes.create_string_buffer(bytes(machine.mem_read(data + 4, 84)))
                self.assertEqual(bool(LIB.gobt_pair_scan_record(record, 84)), matches)
                # Model a native API's result without overriding errors.
                machine.reg_write(reg.UC_MIPS_REG_V0, result)

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.emu_start(method, stop, count=32)
        self.assertEqual(seen, [syscall])
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), stack)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_RA), stop)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_S0), 0x13572468)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), result)
        self.assertEqual(bytes(cpu.mem_read(data, 88)), bytes(obj))

    def test_matching_registration_slot(self):
        self.exercise("DualSense Wireless Controller", True, 1)

    def test_matching_registration_error(self):
        self.exercise("DualSense Wireless Controller", True, 0x802F0140)

    def test_unrelated_device(self):
        self.exercise("Other Controller", False, 2)

    def test_similar_name_unchanged(self):
        self.exercise("DualSenseFake", False, 0x802F0135)


if __name__ == "__main__":
    unittest.main()
