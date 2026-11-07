"""Run the compiled PSP input bridge with mocked kernel/CDC calls in Unicorn."""
from pathlib import Path
import struct
import subprocess
import unittest
import zlib

from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_LITTLE_ENDIAN, UC_HOOK_CODE
from unicorn import mips_const as reg

ELF = Path("pspgobt_probe.elf").read_bytes()
SYMBOLS = {}
for line in subprocess.check_output(["psp-nm", "pspgobt_probe.elf"], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3:
        SYMBOLS[fields[2]] = int(fields[0], 16)
# Release compiles the logger out. Its optional mock address is never called.
SYMBOLS.setdefault("gobt_log", 0x0A700E00)


def machine():
    cpu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN)
    cpu.mem_map(0, 0x10000)
    section_offset = struct.unpack_from("<I", ELF, 32)[0]
    section_size, section_count = struct.unpack_from("<HH", ELF, 46)
    for i in range(section_count):
        _, kind, flags, address, offset, size, *_ = struct.unpack_from("<10I", ELF, section_offset + i * section_size)
        if flags & 2 and kind != 8 and size:
            cpu.mem_write(address, ELF[offset:offset + size])
    cpu.mem_map(0x0A600000, 0x10000)
    cpu.mem_map(0x0A700000, 0x1000)
    cpu.reg_write(reg.UC_MIPS_REG_SP, 0x0A60F000)
    cpu.reg_write(reg.UC_MIPS_REG_RA, 0x0A700F00)
    # Allegrex MIN/MAX implement the signed length clamps. Unicorn's generic
    # MIPS32 CPU lacks these PSP instructions; emulate their signed semantics.
    def allegrex(m, address, size, _):
        pointer = address & 0x1FFFFFFF if 0x80000000 <= address < 0xC0000000 else address
        instruction = struct.unpack("<I", m.mem_read(pointer, 4))[0]
        operation = instruction & 0xFC0007FF
        if operation in (0x0000002C, 0x0000002D):
            rs, rt, rd = (instruction >> 21) & 31, (instruction >> 16) & 31, (instruction >> 11) & 31
            signed = lambda value: value - 0x100000000 if value & 0x80000000 else value
            choose = max if operation == 0x2C else min
            value = choose(signed(m.reg_read(reg.UC_MIPS_REG_0 + rs)),
                           signed(m.reg_read(reg.UC_MIPS_REG_0 + rt)))
            m.reg_write(reg.UC_MIPS_REG_0 + rd, value & 0xFFFFFFFF)
            m.reg_write(reg.UC_MIPS_REG_PC, address + 4)
    cpu.hook_add(UC_HOOK_CODE, allegrex)
    return cpu


def word(cpu, symbol, value):
    cpu.mem_write(SYMBOLS[symbol], struct.pack("<I", value))


class CompiledBridge(unittest.TestCase):
    def exercise(self, first, matching=True, matching_context=True, size=64, result=0):
        cpu = machine()
        context, buffer, callback, native = 0x0A600000, 0x0A600100, 0x0A700100, 0x0A700000
        cpu.mem_write(context, struct.pack("<IHH", 2, 1, 1))
        cpu.mem_write(buffer, b"\xA5" * 128)
        cpu.mem_write(SYMBOLS["contexts"], struct.pack("<II", context, context + 48))
        cpu.mem_write(SYMBOLS["generations"], struct.pack("<II", 7, 0))
        word(cpu, "original_receive", callback)
        word(cpu, "original_submit", native)
        args = [2, buffer, size, callback if matching else callback + 4,
                context if matching_context else context + 64]
        for r, value in zip((reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2,
                             reg.UC_MIPS_REG_A3, reg.UC_MIPS_REG_T0), args):
            cpu.reg_write(r, value)
        seen = []

        def observe(m, address, _, __):
            returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                       SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                       SYMBOLS["sceKernelGetSystemTimeWide"]: 1000,
                       SYMBOLS["gobt_log"]: 0}
            if address == native:
                actual = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                          reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3, reg.UC_MIPS_REG_T0)]
                expected = args.copy()
                if matching and matching_context and size == 64:
                    expected[3] = SYMBOLS["receive_input"]
                self.assertEqual(actual, expected)
                seen.append(actual)
                returns[address] = result
            if address == SYMBOLS.get("memcpy"):
                dst, src, count = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2)]
                m.mem_write(dst, bytes(m.mem_read(src, count)))
                returns[address] = dst
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_V1, 0)
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.emu_start(SYMBOLS["submit_first" if first else "submit_next"], 0x0A700F00, count=10000)
        self.assertEqual(len(seen), 1)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), result)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)
        self.assertEqual(bytes(cpu.mem_read(buffer, 128)), b"\xA5" * 128)
        generation = struct.unpack("<I", cpu.mem_read(SYMBOLS["generations"], 4))[0]
        self.assertEqual(generation, 8 if first and matching and matching_context and size == 64 else 7)
        if first and matching and matching_context and size == 64:
            status = struct.unpack("<I", cpu.mem_read(SYMBOLS["queue"] + 20, 4))[0]
            self.assertEqual(status, result)

    def test_first_read_preserves_five_arguments(self):
        self.exercise(True)

    def test_requeued_read_preserves_five_arguments(self):
        self.exercise(False)

    def test_unrelated_callback(self):
        self.exercise(True, matching=False)

    def test_unrelated_context(self):
        self.exercise(True, matching_context=False)

    def test_unexpected_size(self):
        self.exercise(True, size=32)

    def test_native_error_preserved(self):
        self.exercise(True, result=0x802F0208)

    def test_cancel_callback_cannot_requeue_dma_during_context_reset(self):
        cpu = machine()
        context, callback = 0x0A600000, 0x0A700100
        word(cpu, "contexts", context)
        word(cpu, "original_receive", callback)
        word(cpu, "resetting", 1)
        for r, value in zip((reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                             reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3, reg.UC_MIPS_REG_T0),
                            (2, 0x0A600100, 64, callback, context)):
            cpu.reg_write(r, value)
        cpu.emu_start(SYMBOLS["submit_next"], 0x0A700F00, count=10000)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), 0xFFFFFFFF)
        self.assertEqual(struct.unpack("<I", cpu.mem_read(SYMBOLS["queued"], 4))[0], 0)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)

    def test_xmb_connect_forces_generic_only_for_classified_dualsense(self):
        for selected, mode, result in ((1, 1, 0), (1, 1, 0x802F0140), (0, 1, 0), (-1, 1, 0)):
            with self.subTest(selected=selected, result=result):
                cpu = machine()
                native, calls = 0x0A700100, []
                # TraceApi has six 32-bit fields; CONNECT is index 3.
                cpu.mem_write(SYMBOLS["apis"] + 3 * 24 + 12, struct.pack("<I", native))
                cpu.reg_write(reg.UC_MIPS_REG_K1, 0x12340000)

                def observe(m, address, _, __):
                    returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                               SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                               SYMBOLS["gobt_log"]: 0,
                               SYMBOLS["gobt_input_bridge_connected"]: 0}
                    if address == SYMBOLS["gobt_input_bridge_prepare"]:
                        self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), 1)
                        calls.append("classify")
                        returns[address] = selected & 0xFFFFFFFF
                    if address == native:
                        self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), 1)
                        self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A1), 0 if selected == 1 else mode)
                        self.assertEqual(m.reg_read(reg.UC_MIPS_REG_K1), 0x12340000)
                        calls.append("connect")
                        returns[address] = result
                    if address in returns:
                        m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                        m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

                cpu.hook_add(UC_HOOK_CODE, observe)
                cpu.reg_write(reg.UC_MIPS_REG_A0, 1)
                cpu.reg_write(reg.UC_MIPS_REG_A1, mode)
                cpu.emu_start(SYMBOLS["pspgobtTraceConnect"], 0x0A700F00, count=10000)
                self.assertEqual(calls, ["classify", "connect"])
                self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), result)
                self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_K1), 0x12340000)

    def output(self, result, ready=True):
        cpu = machine()
        context, flags, native = 0x0A600000, 0x0A700200, 0x0A700300
        cpu.mem_write(context, struct.pack("<IHH", 2, 1, 1))
        cpu.mem_write(SYMBOLS["contexts"], struct.pack("<II", context, context + 48))
        word(cpu, "generations", 7)
        word(cpu, "reads_ready", int(ready))
        word(cpu, "connection_flags", flags)
        word(cpu, "write_hid", native)
        cpu.mem_write(SYMBOLS["device_models"] + 4, struct.pack("<I", 1))  # DualSense in slot 1
        source = SYMBOLS["sources"]
        cpu.mem_write(source + 8, struct.pack("<I", 1))  # accepted
        cpu.reg_write(reg.UC_MIPS_REG_A0, 0)
        cpu.reg_write(reg.UC_MIPS_REG_A1, 1)
        cpu.reg_write(reg.UC_MIPS_REG_A2, source)
        seen = []

        def observe(m, address, _, __):
            returns = {flags: 0x202, SYMBOLS["sceKernelGetSystemTimeWide"]: 1000,
                       SYMBOLS["gobt_log"]: 0}
            if address == SYMBOLS.get("memset"):
                dst, value, count = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2)]
                m.mem_write(dst, bytes([value & 255]) * count)
                returns[address] = dst
            if address == native:
                channel, pointer, size, timeout = [m.reg_read(r) for r in
                    (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3)]
                self.assertEqual((channel, pointer % 64, size), (2, 0, 79))
                self.assertEqual(struct.unpack("<I", m.mem_read(timeout, 4))[0], 200000)
                packet = bytes(m.mem_read(pointer, size))
                self.assertEqual(packet[:4], bytes([0xA2, 0x31, 0, 0x10]))
                self.assertEqual(zlib.crc32(packet[:-4]), struct.unpack("<I", packet[-4:])[0])
                seen.append(packet)
                returns[address] = result
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_V1, 0)
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.emu_start(SYMBOLS["initialize"], 0x0A700F00, count=100000)
        self.assertEqual(len(seen), int(ready))
        initialized, attempts = struct.unpack("<II", cpu.mem_read(source + 20, 8))
        self.assertEqual((initialized, attempts),
                         (int(result == 79), 1) if ready else (0, 0))
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)

    def test_aligned_complete_output_frame(self):
        self.output(79)

    def test_failed_output_does_not_mark_initialized(self):
        self.output(0x802F0208)

    def test_short_output_does_not_mark_initialized(self):
        self.output(78)
        self.output(0)

    def test_output_waits_for_receive_setup(self):
        self.output(79, ready=False)

    def pending_read(self, kind="native", matching_context=True, matching_buffer=True):
        cpu = machine()
        # Unicorn translates KSEG0 CPU loads to physical RAM addresses.
        cpu.mem_map(0x08200000, 0x10000)
        context, states, entry, data = 0x88201000, 0x88202000, 0x88203000, 0x88204000
        physical = lambda pointer: pointer & 0x1FFFFFFF
        callback = 0x0A700100
        word(cpu, "original_receive", callback)
        word(cpu, "cdc_read_states", states)
        word(cpu, "contexts", context)
        word(cpu, "generations", 7)
        cpu.mem_write(physical(context), struct.pack("<IHH", 2, 1, 1))
        cpu.mem_write(physical(context + 40), struct.pack("<I", entry))
        cpu.mem_write(physical(entry + 8), struct.pack("<I", data))
        cpu.mem_write(physical(data), b"\xA5" * 64)
        initial = {"native": callback, "ours": SYMBOLS["receive_input"],
                   "other": callback + 4, "none": 0}[kind]
        slot = states + 2 * 12 + 0x7C
        original = struct.pack("<III", initial, data if matching_buffer else data + 64,
                               context if matching_context else context + 48)
        cpu.mem_write(physical(slot), original)
        cpu.reg_write(reg.UC_MIPS_REG_A0, 0)
        cpu.reg_write(reg.UC_MIPS_REG_A1, 1)

        def observe(m, address, _, __):
            returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                       SYMBOLS["sceKernelCpuResumeIntr"]: 0, SYMBOLS["gobt_log"]: 0}
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.emu_start(SYMBOLS["attach_pending_read"], 0x0A700F00, count=10000)
        adopted = kind == "native" and matching_context and matching_buffer
        expected = bytearray(original)
        if adopted:
            struct.pack_into("<I", expected, 0, SYMBOLS["receive_input"])
        self.assertEqual(bytes(cpu.mem_read(physical(slot), 12)), bytes(expected))
        self.assertEqual(bytes(cpu.mem_read(physical(data), 64)), b"\xA5" * 64)
        self.assertEqual(struct.unpack("<I", cpu.mem_read(SYMBOLS["generations"], 4))[0], 8 if adopted else 7)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0),
                         int(adopted or kind == "ours" and matching_context and matching_buffer))
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)

    def test_attach_existing_native_read(self):
        self.pending_read()

    def test_do_not_attach_other_context(self):
        self.pending_read(matching_context=False)

    def test_do_not_attach_other_buffer(self):
        self.pending_read(matching_buffer=False)

    def test_do_not_replace_unrelated_callback(self):
        self.pending_read(kind="other")

    def test_no_pending_read_stays_unmodified(self):
        self.pending_read(kind="none")

    def test_already_attached_read_is_idempotent(self):
        self.pending_read(kind="ours")

    def wide_submit(self, first=True, ownership=True, result=0):
        cpu = machine()
        cpu.mem_map(0x08200000, 0x10000)
        context, entry, data, native, callback = 0x88201000, 0x88202000, 0x88203000, 0x0A700100, 0x0A700200
        physical = lambda p: p & 0x1FFFFFFF
        cpu.mem_write(physical(context), struct.pack("<IHH", 2, 1, 1))
        cpu.mem_write(physical(context + 40), struct.pack("<I", entry))
        cpu.mem_write(physical(entry + 8), struct.pack("<I", data if ownership else data + 64))
        cpu.mem_write(physical(data), b"\xCC" * 128)
        for name, value in (("contexts", context), ("original_submit", native),
                            ("original_receive", callback), ("installed", 1),
                            ("discovery_bypassed", 1), ("generations", 7),
                            ("reads_ready", 0 if first else 1)):
            word(cpu, name, value)
        for r, value in zip((reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2,
                             reg.UC_MIPS_REG_A3, reg.UC_MIPS_REG_T0), (2, data, 64, callback, context)):
            cpu.reg_write(r, value)
        calls = []

        def observe(m, address, _, __):
            returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                       SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                       SYMBOLS["sceKernelGetSystemTimeWide"]: 1000, SYMBOLS["gobt_log"]: 0}
            if address == native:
                args = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                        reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3, reg.UC_MIPS_REG_T0)]
                self.assertEqual(args, [2, SYMBOLS["receive_buffers"] if ownership else data,
                                       128 if ownership else 64,
                                       SYMBOLS["receive_wide" if ownership else "receive_input"], context])
                self.assertEqual(args[1] % 64, 0)
                calls.append(args)
                returns[address] = result
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_V1, 0)
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.emu_start(SYMBOLS["submit_first" if first else "submit_next"], 0x0A700F00, count=20000)
        self.assertEqual(len(calls), 1)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), result)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)
        self.assertEqual(bytes(cpu.mem_read(physical(data), 128)), b"\xCC" * 128)
        self.assertEqual(struct.unpack("<I", cpu.mem_read(SYMBOLS["wide_active"], 4))[0],
                         int(ownership and not result & 0x80000000))
        self.assertEqual(struct.unpack("<I", cpu.mem_read(SYMBOLS["reads_ready"], 4))[0],
                         int(not result & 0x80000000))

    def test_first_selected_read_uses_128_byte_staging(self):
        self.wide_submit()

    def test_reposted_selected_read_uses_128_byte_staging(self):
        self.wide_submit(first=False)

    def test_buffer_ownership_mismatch_keeps_original_64_byte_request(self):
        self.wide_submit(ownership=False)

    def test_wide_submission_failure_preserves_error_and_clears_active(self):
        self.wide_submit(result=0x80300009)

    def test_wide_repost_failure_clears_existing_readiness(self):
        self.wide_submit(first=False, result=0x80300009)

    def discovery(self, prepared=True, connected=True, matching=True, suspended=False, installed=True, device=1):
        cpu = machine()
        context, native, flags = 0x0A600000, 0x0A700100, 0x0A700200
        descriptor = context + 8 if matching else context + 56
        cpu.mem_write(context, struct.pack("<IHH", 2, device, 1))
        word(cpu, "contexts", context)
        word(cpu, "native_register", native)
        word(cpu, "connection_flags", flags)
        word(cpu, "installed", int(installed))
        word(cpu, "suspended", int(suspended))
        word(cpu, "prepared_devices", 2 if prepared else 0)
        cpu.reg_write(reg.UC_MIPS_REG_K1, 0x12340000)
        cpu.reg_write(reg.UC_MIPS_REG_A0, descriptor)
        seen = []
        bypass = prepared and connected and matching and not suspended and installed and device == 1

        def observe(m, address, _, __):
            returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                       SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                       SYMBOLS["gobt_log"]: 0, flags: 0x202 if connected else 0}
            if address == native:
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), descriptor)
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_K1), 0x12340000)
                seen.append(descriptor)
                returns[address] = 0x80310102
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.emu_start(SYMBOLS["register_hid_service"], 0x0A700F00, count=10000)
        self.assertEqual(len(seen), 0 if bypass else 1)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), 0 if bypass else 0x80310102)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_K1), 0x12340000)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)

    def test_selected_dualsense_skips_native_discovery(self):
        self.discovery()

    def test_unprepared_device_retains_native_discovery_error(self):
        self.discovery(prepared=False)

    def test_disconnected_device_retains_native_discovery(self):
        self.discovery(connected=False)

    def test_unrelated_descriptor_retains_native_discovery(self):
        self.discovery(matching=False)

    def test_suspended_device_retains_native_discovery(self):
        self.discovery(suspended=True)

    def test_stopped_bridge_retains_native_discovery(self):
        self.discovery(installed=False)

    def test_invalid_device_retains_native_discovery(self):
        self.discovery(device=9)

    def prepare(self, result=0, matching=True, power_change=False):
        cpu = machine()
        native = 0x0A700100
        word(cpu, "get_info", native)
        word(cpu, "installed", 1)
        word(cpu, "prepared_devices", 6)
        cpu.reg_write(reg.UC_MIPS_REG_K1, 0x12340000)
        cpu.reg_write(reg.UC_MIPS_REG_A0, 1)
        record = bytearray(80)
        struct.pack_into("<I", record, 0, 80)
        name = ("DualSense Wireless Controller" if matching else "Other Controller").encode("utf-16le")
        record[6:6 + len(name)] = name
        struct.pack_into("<H", record, 72, 0x402)
        queries = []

        def observe(m, address, _, __):
            returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                       SYMBOLS["sceKernelCpuResumeIntr"]: 0, SYMBOLS["gobt_log"]: 0}
            if address == native:
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), 1)
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_K1), 0)
                m.mem_write(m.reg_read(reg.UC_MIPS_REG_A1), bytes(record))
                if power_change:
                    word(m, "power_generation", 1)
                queries.append(1)
                returns[address] = result
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.emu_start(SYMBOLS["gobt_input_bridge_prepare"], 0x0A700F00, count=10000)
        selected = result == 0 and matching and not power_change
        self.assertEqual(queries, [1])
        self.assertEqual(struct.unpack("<I", cpu.mem_read(SYMBOLS["prepared_devices"], 4))[0],
                         6 if selected else 4)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_K1), 0x12340000)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)

    def test_prepare_selects_dualsense_metadata(self):
        self.prepare()

    def test_prepare_failed_query_clears_previous_selection(self):
        self.prepare(result=0x802F0203)

    def test_prepare_different_device_clears_previous_selection(self):
        self.prepare(matching=False)

    def test_prepare_power_change_clears_previous_selection(self):
        self.prepare(power_change=True)

    def test_worker_keeps_prepared_selection_while_native_setup_runs(self):
        cpu = machine()
        provider = 0x0A600100
        cpu.mem_write(provider + 44, struct.pack("<I", 7))  # SceModule.modid
        word(cpu, "installed", 1)
        word(cpu, "provider_uid", 7)
        word(cpu, "cdc_provider_uid", 7)
        word(cpu, "cdc_read_states", 0x23A0)
        word(cpu, "prepared_devices", 2)
        word(cpu, "next_link_poll", 1000)
        resumed = []

        def observe(m, address, _, __):
            returns = {SYMBOLS["sceKernelFindModuleByName"]: provider,
                       SYMBOLS["sceKernelGetSystemTimeWide"]: 0,
                       SYMBOLS["sceKernelCpuSuspendIntr"]: 1}
            if address == SYMBOLS["sceKernelCpuResumeIntr"]:
                resumed.append(1)
                m.emu_stop()  # one worker queue snapshot, before idle work
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_V1, 0)  # high word of the mocked 64-bit clock
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.emu_start(SYMBOLS["worker"], 0x0A700F00, count=10000)
        self.assertEqual(resumed, [1])
        self.assertEqual(struct.unpack("<I", cpu.mem_read(SYMBOLS["prepared_devices"], 4))[0], 2)

    def test_power_events_clear_prepared_selection(self):
        for event in (0x400, 0x4000, 0x400000):
            with self.subTest(event=event):
                cpu = machine()
                word(cpu, "prepared_devices", 2)
                cpu.reg_write(reg.UC_MIPS_REG_A0, event)

                def observe(m, address, _, __):
                    returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                               SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                               SYMBOLS["pspgobtDisconnect"]: 0}
                    if address in returns:
                        m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                        m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

                cpu.hook_add(UC_HOOK_CODE, observe)
                cpu.emu_start(SYMBOLS["bridge_power_event"], 0x0A700F00, count=10000)
                self.assertEqual(struct.unpack("<I", cpu.mem_read(SYMBOLS["prepared_devices"], 4))[0], 0)

    def test_native_did_trace_preserves_arguments_outputs_and_error(self):
        cpu = machine()
        native, vendor, product, extra = 0x0A700100, 0x0A600100, 0x0A600102, 0x0A600104
        args = [1, vendor, product, extra]
        word(cpu, "native_identity", native)
        cpu.reg_write(reg.UC_MIPS_REG_K1, 0x12340000)
        for r, value in zip((reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                             reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3), args):
            cpu.reg_write(r, value)
        seen = []

        def observe(m, address, _, __):
            returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                       SYMBOLS["sceKernelCpuResumeIntr"]: 0, SYMBOLS["gobt_log"]: 0}
            if address == native:
                self.assertEqual([m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                                  reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3)], args)
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_K1), 0x12340000)
                m.mem_write(vendor, struct.pack("<HHI", 0x054C, 0x0CE6, 0x12345678))
                seen.append(1)
                returns[address] = 0x802F0153
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.emu_start(SYMBOLS["trace_native_identity"], 0x0A700F00, count=10000)
        self.assertEqual(seen, [1])
        self.assertEqual(bytes(cpu.mem_read(vendor, 8)), struct.pack("<HHI", 0x054C, 0x0CE6, 0x12345678))
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), 0x802F0153)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_K1), 0x12340000)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)

    def native_setup(self, prepared, flags=0x202):
        # Optional local integration fixture. Sony executable captures are
        # never included in the repository or release archives.
        capture = Path("pspgobt/sceBSMan-04540F41.text.bin")
        if not capture.exists():
            self.skipTest("local BSMan firmware capture not present")
        cpu = machine()
        cpu.mem_map(0x08200000, 0x100000)
        base = 0x88238900
        physical = lambda pointer: pointer & 0x1FFFFFFF
        cpu.mem_write(physical(base), capture.read_bytes())
        context, entry, data = base + 0xDE78, 0x88249000, 0x8824A000
        cpu.mem_write(physical(entry), struct.pack("<III", 0, 0, data))
        word(cpu, "contexts", context)
        word(cpu, "installed", 1)
        word(cpu, "prepared_devices", 2 if prepared else 0)
        word(cpu, "connection_flags", base + 0x7E0)
        word(cpu, "native_identity", base + 0x58D8)
        word(cpu, "native_register", base + 0xCBF8)
        word(cpu, "original_receive", base + 0x9EE0)
        word(cpu, "original_submit", 0x0A700100)
        # These in-window test trampolines compensate for ELF relocation at
        # zero. Real PSP builds place both BSMan and the plugin in KSEG0.
        trampolines = {0x88248000: SYMBOLS["trace_native_identity"],
                       0x88248010: SYMBOLS["register_hid_service"],
                       0x88248020: SYMBOLS["submit_first"],
                       0x88248030: SYMBOLS["native_setup_flags"]}
        for offset, target in ((0xA1E4, 0x88248000), (0xA1FC, 0x88248010),
                               (0xA22C, 0x88248020), (0xA13C, 0x88248030)):
            cpu.mem_write(physical(base + offset), struct.pack("<I", 0x0C000000 | ((target >> 2) & 0x03FFFFFF)))
        cpu.reg_write(reg.UC_MIPS_REG_A0, 1)
        if prepared:
            # A failed original helper leaves these locals uninitialized.
            # Poison them with DS3 IDs: the bypass must clear both so Sony's
            # DS3-specific branch is never entered for the DualSense.
            cpu.mem_write(0x0A60EFE0, struct.pack("<HH", 0x054C, 0x0268))
        reads, registrations, did_queries = [], [], []

        def observe(m, address, _, __):
            if address in trampolines:
                m.reg_write(reg.UC_MIPS_REG_PC, trampolines[address])
                return
            returns = {base + 0x7E0: flags, base + 0xCA78: 1, base + 0xCA80: 0,
                       base + 0x5E50: 0, base + 0xA560: 0, base + 0x58D8: 0x802F0153,
                       SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                       SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                       SYMBOLS["sceKernelGetSystemTimeWide"]: 1000,
                       SYMBOLS["gobt_log"]: 0}
            if address == base + 0xA424:
                m.mem_write(m.reg_read(reg.UC_MIPS_REG_A0), struct.pack("<I", entry))
                returns[address] = 0
            if address == base + 0x58D8:
                did_queries.append(1)
                if prepared:
                    # Reproduce the disappearing context seen while the old
                    # startup waited for a remote DID query in the 0.12 log.
                    m.mem_write(physical(context + 4), b"\0\0")
            if address == base + 0xCBF8:
                registrations.append(m.reg_read(reg.UC_MIPS_REG_A0))
                returns[address] = 0x80310102
            if address == 0x0A700100:
                actual = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1,
                          reg.UC_MIPS_REG_A2, reg.UC_MIPS_REG_A3, reg.UC_MIPS_REG_T0)]
                self.assertEqual(actual, [2, SYMBOLS["receive_buffers"], 128, SYMBOLS["receive_wide"], context])
                reads.append(actual)
                returns[address] = 0
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_V1, 0)
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.emu_start(base + 0xA128, 0x0A700F00, count=100000)
        self.assertEqual(len(reads), int(prepared))
        self.assertEqual(registrations, [] if prepared else [context + 8])
        self.assertEqual(did_queries, [] if prepared else [1])
        if prepared:
            self.assertEqual(bytes(cpu.mem_read(0x0A60EFE0, 4)), b"\0" * 4)
            self.assertEqual(struct.unpack("<H", cpu.mem_read(physical(context + 6), 2))[0], 1)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), 0 if prepared else 0x80310102)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)

    def test_captured_native_setup_reaches_first_read_after_bypass(self):
        self.native_setup(prepared=True)

    def test_captured_native_setup_preserves_other_device_service_error(self):
        self.native_setup(prepared=False)

    def test_captured_native_setup_uses_generic_mode_even_with_alternate_link_flags(self):
        self.native_setup(prepared=True, flags=0x402)

    def test_captured_alternate_mode_is_preserved_for_other_controllers(self):
        self.native_setup(prepared=False, flags=0x402)

    def test_captured_local_cleanup_only_cancels_read_and_releases_context(self):
        capture = Path("pspgobt/sceBSMan-04540F41.text.bin")
        if not capture.exists():
            self.skipTest("local BSMan firmware capture not present")
        cpu = machine()
        cpu.mem_map(0x08200000, 0x100000)
        physical = lambda pointer: pointer & 0x1FFFFFFF
        base, context = 0x88238900, 0x88246778
        cpu.mem_write(physical(base), capture.read_bytes())
        cpu.mem_write(physical(context), struct.pack("<IHH", 2, 1, 1) + bytes(40))
        cpu.mem_write(physical(context + 40), struct.pack("<I", 0xDEADBEEF))
        calls = []

        def observe(m, address, _, __):
            returns = {base + 0xCA78: 1, base + 0xCA80: 0, base + 0x5E50: 0}
            if address in (base + 0x2B10, base + 0x3720):
                self.fail("Local HID cleanup must not send Bluetooth commands")
            if address == base + 0xCBE8:
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), context + 8)
                calls.append("unregister-local-service")
                returns[address] = 0
            if address == base + 0xCC88:
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), 2)
                calls.append("cancel-read-channel-2")
                returns[address] = 0
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.reg_write(reg.UC_MIPS_REG_A0, 1)
        cpu.emu_start(base + 0xA2DC, 0x0A700F00, count=100000)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), 0)
        self.assertEqual(calls, ["unregister-local-service", "cancel-read-channel-2"])
        self.assertEqual(bytes(cpu.mem_read(physical(context), 6)), bytes(6))
        self.assertEqual(struct.unpack("<I", cpu.mem_read(physical(context + 40), 4))[0], 0xDEADBEEF)

    def identify_prepared(self, matching=True, connected=True, query=0, suspended=False, power_change=False):
        cpu = machine()
        info, identity, flags = 0x0A700100, 0x0A700200, 0x0A700300
        for name, value in (("get_info", info), ("connection_flags", flags),
                            ("prepared_devices", 2), ("installed", 1), ("suspended", int(suspended))):
            word(cpu, name, value)
        cpu.reg_write(reg.UC_MIPS_REG_A0, 1)
        cpu.reg_write(reg.UC_MIPS_REG_K1, 0x12340000)
        record = bytearray(80)
        struct.pack_into("<I", record, 0, 80)
        name = ("DualSense Wireless Controller" if matching else "Other Controller").encode("utf-16le")
        record[6:6 + len(name)] = name
        struct.pack_into("<H", record, 72, 0x402)
        seen = []

        def observe(m, address, _, __):
            returns = {flags: 0x202 if connected else 0, SYMBOLS["gobt_log"]: 0}
            if address == info:
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), 1)
                m.mem_write(m.reg_read(reg.UC_MIPS_REG_A1), bytes(record))
                if power_change:
                    word(m, "power_generation", 1)
                    word(m, "prepared_devices", 0)
                seen.append(1)
                returns[address] = query
            if address == identity:
                self.fail("Prepared source must not start a second DID query")
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.emu_start(SYMBOLS["identify"], 0x0A700F00, count=10000)
        self.assertEqual(seen, [] if suspended else [1])
        selected = matching and connected and query == 0 and not suspended and not power_change
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), 3 if selected else 0)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_K1), 0x12340000)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)

    def test_prepared_identity_skips_remote_did_query(self):
        self.identify_prepared()

    def test_prepared_identity_refuses_changed_metadata(self):
        self.identify_prepared(matching=False)

    def test_prepared_identity_refuses_disconnected_source(self):
        self.identify_prepared(connected=False)

    def test_prepared_identity_refuses_failed_record_query(self):
        self.identify_prepared(query=0x802F0141)

    def test_prepared_identity_refuses_suspended_source(self):
        self.identify_prepared(suspended=True)

    def test_prepared_identity_refuses_power_transition(self):
        self.identify_prepared(power_change=True)


if __name__ == "__main__":
    unittest.main()
