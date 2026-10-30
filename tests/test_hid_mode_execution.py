"""Run saved-device creation, method replacement and labels in captured XMB code."""
import ctypes
from pathlib import Path
import struct
import unittest
from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_LITTLE_ENDIAN, UC_HOOK_CODE
from unicorn import mips_const as reg
from test_route_emulation import LIB

for name in ("gobt_build_stored_hid_code",):
    getattr(LIB, name).argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
for name in ("gobt_build_hid_label_code", "gobt_build_hid_store_code"):
    getattr(LIB, name).argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32, ctypes.c_uint32]
LIB.gobt_hid_mode_supported.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
LIB.gobt_route_hid_cached.argtypes = [ctypes.c_void_p, ctypes.c_size_t]


class SavedDevice:
    def __init__(self, case, base, capture, kind=8, selected=True, error=0, known=False):
        self.case, self.base = case, base
        self.kind, self.selected, self.error, self.known = kind, selected, error, known
        self.data, self.stack, self.stop, self.decision = 0x09E00000, 0x09F00F00, 0x0A562000, 0x0A561000
        self.cpu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN)
        self.cpu.mem_map(0x0A560000, 0x50000)
        self.cpu.mem_map(self.data, 0x2000)
        self.cpu.mem_map(0x09F00000, 0x1000)
        self.cpu.mem_write(base, capture)
        self.objects, self.freed, self.labels, self.decisions = [], [], [], []
        self.caps = 0x402 if selected else 1
        self.check_gate, self.gate_result = False, None
        self.cpu.hook_add(UC_HOOK_CODE, self.observe)
        for name, count, address, args in (
            ("gobt_build_stored_hid_code", 29, 0x0A560000, (self.decision, base)),
            ("gobt_build_hid_label_code", 12, 0x0A560200, (base,)),
            ("gobt_build_hid_store_code", 8, 0x0A560400, (base,)),
        ):
            words = (ctypes.c_uint32 * count)()
            case.assertEqual(getattr(LIB, name)(words, address, *args), 0)
            self.cpu.mem_write(address, bytes(words))
        registration = (ctypes.c_uint32 * 21)()
        case.assertEqual(LIB.gobt_build_route_code(registration, 0x0A560600, self.decision + 0x100,
                                                  base + 0x14E0C, base + 0x14F64), 0)
        self.cpu.mem_write(0x0A560600, bytes(registration))
        self.cpu.mem_write(base + 0x1512C, struct.pack("<I", 0x0C000000 | (0x0A560600 >> 2)))
        self.cpu.mem_write(base + 0x151DC, struct.pack("<I", 0x0C000000 | (0x0A560000 >> 2)))
        for offset, address in ((0x146C4, 0x0A560200), (0x14D34, 0x0A560400)):
            self.cpu.mem_write(base + offset, struct.pack("<II", 0x08000000 | (address >> 2), 0))

    def put(self, address, *values):
        self.cpu.mem_write(address, struct.pack("<" + "I" * len(values), *values))

    def word(self, address):
        return struct.unpack("<I", self.cpu.mem_read(address, 4))[0]

    def observe(self, m, address, _, __):
        a, b, c = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2)]
        result = None
        if self.check_gate and address in (self.base + 0xE004, self.base + 0xE06C):
            self.gate_result = address == self.base + 0xE004
            m.emu_stop()
            return
        if address == self.decision + 0x100:
            profile = ctypes.create_string_buffer(bytes(m.mem_read(a, 44)))
            info = ctypes.create_string_buffer(bytes(m.mem_read(b, 80)))
            result = LIB.gobt_route_hid_record(profile, 44, info, 80)
            m.mem_write(a, profile.raw[:44])
        elif address == self.base + 0x14C5C:
            result = self.kind  # registry reader, not a Bluetooth query
        elif address in (self.base + 0x1322C, self.base + 0x133B0):
            result = 0  # native name/string storage
        elif address == self.base + 0x1AAB0:
            self.case.assertEqual(a, 3)
            if not self.error:
                info = bytearray(80)
                struct.pack_into("<IH", info, 0, 80, 3)
                name = ("DualSense Wireless Controller" if self.selected else "Audio Device").encode("utf-16le")
                info[6:6 + len(name)] = name
                struct.pack_into("<H", info, 72, self.caps)
                m.mem_write(b, bytes(info))
            result = self.error
        elif address == self.decision:
            self.case.assertEqual((a, c), (self.data, 3))
            profile = ctypes.create_string_buffer(bytes(m.mem_read(a, 44)))
            if b:
                self.case.assertEqual(self.error, 0)
                info = ctypes.create_string_buffer(bytes(m.mem_read(b, 80)))
                result = LIB.gobt_route_hid_record(profile, 44, info, 80)
            else:
                self.case.assertNotEqual(self.error, 0)
                result = LIB.gobt_route_hid_cached(profile, 44) if self.known else 0
            m.mem_write(a, profile.raw[:44])
            self.decisions.append(result)
            for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3):
                m.reg_write(r, 0xBADCAFE)
        elif address == self.base + 0x1ABE0:
            self.case.assertEqual(a, 4)
            result = self.data + 0x200 + 0x40 * len(self.objects)
            self.objects.append(result)
        elif address == self.base + 0x1A734:
            self.case.assertEqual(a, self.data + 28)
            pointer = self.data + 0x400
            count = self.word(a + 4)
            self.put(pointer + count * 4, self.word(b))
            self.put(a, pointer, count + 1, count + 1)
            result = 0
        elif address in (self.base + 0x1AD28, self.base + 0x1AC58):
            self.freed.append(a)
            result = 0
        elif address == self.base + 0x15F00:
            self.labels.append(bytes(m.mem_read(b, 32)).split(b"\0", 1)[0])
            result = 0
        if result is not None:
            m.reg_write(reg.UC_MIPS_REG_V0, result)
            m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

    def run(self, address, a, b, c=0, stop=None):
        self.cpu.reg_write(reg.UC_MIPS_REG_SP, self.stack)
        self.cpu.reg_write(reg.UC_MIPS_REG_RA, self.stop)
        for r, v in zip((reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2), (a, b, c)):
            self.cpu.reg_write(r, v)
        self.cpu.emu_start(address, stop or self.stop, count=20000)
        return self.cpu.reg_read(reg.UC_MIPS_REG_V0)

    def table(self):
        self.case.assertEqual(self.word(self.data + 32), 1)
        return self.word(self.word(self.word(self.data + 28)))


class HidMode(unittest.TestCase):
    def captures(self):
        for uid, base in (("04FE4C4D", 0x0A572000), ("050B8563", 0x0A572100)):
            path = Path(f"pspgobt/bluetooth_plugin_module-{uid}.text.bin")
            if not path.exists():
                self.skipTest("requires local Sony XMB captures")
            yield base, path.read_bytes()

    def test_saved_audio_and_ps3_methods_are_replaced_through_actual_paired_loader(self):
        for base, capture in self.captures():
            for kind in (1, 2, 8):
                with self.subTest(base=base, kind=kind):
                    s = SavedDevice(self, base, capture, kind=kind)
                    self.assertEqual(s.run(base + 0x151A0, s.data, 3), 0)
                    self.assertEqual((s.word(s.data), s.word(s.data + 4)), (3, 9))
                    self.assertEqual(s.table(), base + 0x20C28)
                    self.assertEqual(s.freed, [s.objects[0], s.data + 0x400])
                    self.assertEqual(len(s.objects), 2)
                    self.assertEqual(s.decisions, [1])
                    s.run(base + 0x146C4, s.data, s.data + 0x800)
                    self.assertEqual(s.labels, [b"msg_bt_hid"])

    def test_fresh_saved_object_gets_slot_and_hid_methods_without_native_audio_route(self):
        for base, capture in self.captures():
            s = SavedDevice(self, base, capture, kind=0)
            self.assertEqual(s.run(base + 0x151A0, s.data, 3), 0)
            self.assertEqual((s.word(s.data), s.word(s.data + 4)), (3, 9))
            self.assertEqual(s.table(), base + 0x20C28)
            self.assertEqual((len(s.objects), s.freed), (1, []))

    def test_unrelated_audio_device_keeps_its_audio_methods_and_label(self):
        for base, capture in self.captures():
            s = SavedDevice(self, base, capture, selected=False)
            self.assertEqual(s.run(base + 0x151A0, s.data, 3), 0)
            self.assertEqual(s.word(s.data + 4), 8)
            self.assertEqual(s.table(), base + 0x20C40)
            self.assertEqual(s.freed, [])
            s.run(base + 0x146C4, s.data, s.data + 0x800)
            self.assertEqual(s.labels, [b"msg_bt_audio_device"])

    def test_metadata_error_is_preserved_and_only_known_cache_can_select(self):
        for base, capture in self.captures():
            for known in (False, True):
                s = SavedDevice(self, base, capture, error=0x802F0203, known=known)
                s.put(s.data, 3, 8)
                s.run(base + 0x14F64, s.data, 8)
                s.cpu.reg_write(reg.UC_MIPS_REG_S0, s.data)
                record = s.data + 0x900
                self.assertEqual(s.run(0x0A560000, record, 3), 0x802F0203)
                self.assertEqual(s.word(s.data + 4), 9 if known else 8)
                self.assertEqual(s.table(), base + (0x20C28 if known else 0x20C40))

    def test_custom_mode_is_stored_as_native_hid_and_other_types_are_unchanged(self):
        for base, capture in self.captures():
            for kind in range(10):
                s = SavedDevice(self, base, capture)
                s.run(base + 0x14D34, s.data, 3, kind, stop=base + 0x14D3C)
                self.assertEqual(s.cpu.reg_read(reg.UC_MIPS_REG_A2), 2 if kind == 9 else kind)
                self.assertEqual(s.cpu.reg_read(reg.UC_MIPS_REG_A0), s.data)
                self.assertEqual(s.cpu.reg_read(reg.UC_MIPS_REG_A1), 3)
                self.assertEqual(s.cpu.reg_read(reg.UC_MIPS_REG_SP), s.stack - 160)

    def test_registration_of_scan_type_eight_gets_methods_and_passes_actual_support_gate(self):
        for base, capture in self.captures():
            for kind in (1, 2, 8, 9):
                s = SavedDevice(self, base, capture)
                self.assertEqual(s.run(base + 0x150B0, s.data, 3, kind), 3)
                self.assertEqual((s.word(s.data), s.word(s.data + 4)), (3, 9))
                self.assertEqual(s.table(), base + 0x20C28)
                # This native handler-count branch produces the unsupported UI.
                s.check_gate = True
                s.cpu.reg_write(reg.UC_MIPS_REG_V1, s.data)
                s.cpu.reg_write(reg.UC_MIPS_REG_S2, s.data + 0x1000)
                s.run(base + 0xDFF8, 0, 0)
                self.assertTrue(s.gate_result)

    def test_unrelated_unsupported_registration_still_reaches_native_rejection(self):
        for base, capture in self.captures():
            s = SavedDevice(self, base, capture, selected=False)
            s.caps = 0x402
            self.assertEqual(s.run(base + 0x150B0, s.data, 3, 2), 3)
            self.assertEqual(s.word(s.data + 32), 0)
            s.check_gate = True
            s.cpu.reg_write(reg.UC_MIPS_REG_V1, s.data)
            s.cpu.reg_write(reg.UC_MIPS_REG_S2, s.data + 0x1000)
            s.run(base + 0xDFF8, 0, 0)
            self.assertFalse(s.gate_result)

    def test_registration_metadata_error_is_never_overridden(self):
        for base, capture in self.captures():
            s = SavedDevice(self, base, capture, error=0x802F0135)
            self.assertEqual(s.run(base + 0x150B0, s.data, 3, 8), 0x802F0135)
            self.assertEqual(s.objects, [])

    def test_new_mode_guards_reject_modified_paths(self):
        offsets = (0x153FC, 0x15400, 0x15404, 0x15408, 0x1540C, 0x15410, 0x151A8, 0x151DC, 0x151E0, 0x151E4, 0x15224, 0x15228,
                   0x152D8, 0x152DC, 0x146C4, 0x146C8, 0x14D34, 0x14D38,
                   0x15018, 0x15094, 0x15098, 0x1A7E0, 0x1A7E4, 0x1A7E8, 0x1C6FC)
        for base, capture in self.captures():
            image = ctypes.create_string_buffer(capture)
            self.assertEqual(LIB.gobt_hid_mode_supported(image, len(capture), base), 1)
            for offset in offsets:
                changed = bytearray(capture)
                changed[offset] ^= 1
                image = ctypes.create_string_buffer(bytes(changed))
                self.assertEqual(LIB.gobt_hid_mode_supported(image, len(changed), base), 0)


if __name__ == "__main__":
    unittest.main()
