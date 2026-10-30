"""Execute staging -> decoder -> native DS3 transfer-handler integration."""
import struct
import unittest
import zlib

from unicorn import UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import machine, SYMBOLS, word


class ControllerTransfer(unittest.TestCase):
    def exercise(self, simple=False, corrupt=False, prepared=True, reconnect=False, delayed_reset=False):
        cpu = machine()
        cpu.mem_map(0x08200000, 0x10000)
        physical = lambda p: p & 0x1FFFFFFF if p >= 0x80000000 else p
        context, entry, native_data = 0x88201000, 0x88202000, 0x88203000
        native_receive, info, identity, flags, register, passthrough = (
            0x0A700100, 0x0A700200, 0x0A700300, 0x0A700400, 0x0A700500, 0x0A700600)
        staging = SYMBOLS["receive_buffers"]
        cpu.mem_write(physical(context), struct.pack("<IHH", 2, 1, 1))
        cpu.mem_write(physical(context + 40), struct.pack("<I", entry))
        cpu.mem_write(physical(entry + 8), struct.pack("<I", native_data))
        cpu.mem_write(physical(native_data), b"\xCC" * 128)
        word(cpu, "contexts", context)
        word(cpu, "wide_native", native_data)
        word(cpu, "wide_active", 1)
        word(cpu, "generations", 7)
        word(cpu, "installed", 1)
        word(cpu, "prepared_devices", 2 if prepared else 0)
        for name, pointer in (("original_receive", native_receive), ("get_info", info),
                              ("connection_flags", flags)):
            word(cpu, name, pointer)
        if simple:
            # ID, axes, Up+Cross, L1, PS, analog triggers.
            packet = bytes([0xA1, 1, 11, 22, 33, 44, 0x20, 1, 1, 55, 66])
        else:
            report = bytearray(78)
            report[0] = 0x31
            report[2:8] = bytes([11, 22, 33, 44, 55, 66])
            report[9:12] = bytes([0x20, 1, 1])
            struct.pack_into("<I", report, 74, zlib.crc32(b"\xA1" + report[:74]))
            if corrupt:
                report[20] ^= 1
            packet = b"\xA1" + report
        cpu.mem_write(staging, packet)
        calls, handler, clock, power_registrations = [], {}, [1000], []

        def cstring(m, pointer):
            return bytes(m.mem_read(pointer, 64)).split(b"\0", 1)[0]

        def observe(m, address, _, __):
            returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                       SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                       SYMBOLS["sceKernelGetSystemTimeWide"]: clock[0],
                       SYMBOLS["sceKernelRegisterSysEventHandler"]: 0,
                       SYMBOLS["gobt_log"]: 0, flags: 0x202}
            args = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2)]
            if address == SYMBOLS.get("memset"):
                m.mem_write(physical(args[0]), bytes([args[1] & 255]) * args[2])
                returns[address] = args[0]
            if address == SYMBOLS.get("memcpy"):
                m.mem_write(physical(args[0]), bytes(m.mem_read(physical(args[1]), args[2])))
                returns[address] = args[0]
            if address == native_receive:
                self.assertEqual(args, [native_data, min(len(packet), 64), context])
                self.assertEqual(bytes(m.mem_read(physical(native_data), 128)),
                                 packet[:64] + b"\xCC" * (128 - min(len(packet), 64)))
                returns[address] = 42
            if address == info:
                self.assertEqual(args[0], 1)
                record = bytearray(80)
                struct.pack_into("<I", record, 0, 80)
                name = "DualSense Wireless Controller".encode("utf-16le")
                record[6:6 + len(name)] = name
                struct.pack_into("<H", record, 72, 0x402)
                m.mem_write(args[1], bytes(record))
                returns[address] = 0
            if address == identity:
                self.fail("No input session may issue remote DID discovery")
                returns[address] = 0x802F0153
            if address == SYMBOLS["sceKernelRegisterSysEventHandler"]:
                power_registrations.append(1)
            if address == SYMBOLS["sctrlHENFindFunction"]:
                self.assertEqual(cstring(m, args[0]), b"sceController_Service")
                self.assertEqual(cstring(m, args[1]), b"sceCtrl_driver")
                self.assertIn(args[2], (0xE467BEC8, 0x6C86AF22))
                returns[address] = register if args[2] == 0xE467BEC8 else passthrough
            if address == register:
                self.assertEqual(args[0], 1)
                size, callback = struct.unpack("<II", m.mem_read(args[1], 8))
                self.assertEqual(size, 8)
                self.assertEqual(callback, SYMBOLS["copy_input"])
                handler.update(callback=callback, source=args[2])
                calls.append("register-port-1")
                returns[address] = 0
            if address == passthrough:
                self.assertEqual(args[0], 1)
                calls.append("passthrough-mask-1")
                returns[address] = 0
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_V1, 0)
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        def invoke(name, args):
            cpu.reg_write(reg.UC_MIPS_REG_SP, 0x0A60F000)
            cpu.reg_write(reg.UC_MIPS_REG_RA, 0x0A700F00)
            for r, value in zip((reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2), args):
                cpu.reg_write(r, value)
            cpu.emu_start(SYMBOLS[name], 0x0A700F00, count=200000)
            self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)
            return cpu.reg_read(reg.UC_MIPS_REG_V0)

        cpu.hook_add(UC_HOOK_CODE, observe)
        self.assertEqual(invoke("receive_wide", [staging, len(packet), context]), 42)
        self.assertEqual(bytes(cpu.mem_read(SYMBOLS["queue"] + 32, len(packet))), packet)
        invoke("consume", [SYMBOLS["queue"]])
        if corrupt:
            self.assertEqual(calls, [])
            self.assertEqual(handler, {})
            return
        self.assertEqual(calls, ["register-port-1", "passthrough-mask-1"])
        if reconnect:
            self.assertEqual(invoke("gobt_sink_start", [0]), 1)
            self.assertEqual(calls, ["register-port-1", "passthrough-mask-1"] * 2)
            self.assertEqual(power_registrations, [1])
        if delayed_reset:
            # Firmware clears its port/mask after startup. Maintenance must
            # reinstall both, using the same still-fresh decoded report.
            handler.clear()
            clock[0] = 11000
            invoke("gobt_sink_maintain", [])
            self.assertEqual(len(calls), 2)
            clock[0] = 101000
            invoke("gobt_sink_maintain", [])
            self.assertEqual(calls, ["register-port-1", "passthrough-mask-1"] * 2)
            self.assertIn("callback", handler)
            invoke("gobt_sink_maintain", [])
            self.assertEqual(len(calls), 4)  # throttle repeated worker passes
            clock[0] = 201000
            invoke("gobt_sink_maintain", [])
            self.assertEqual(len(calls), 6)  # also handles later resets
            self.assertEqual(power_registrations, [1])
        destination = 0x0A600800
        cpu.mem_write(destination, struct.pack("<I", 0x12345678) + b"\xCC" * 44)
        self.assertEqual(invoke("copy_input", [handler["source"], destination]), 0)
        frame = bytes(cpu.mem_read(destination, 48))
        timestamp, buttons = struct.unpack_from("<II", frame)
        self.assertEqual((timestamp, buttons), (0x12345678, 0x14410))  # PS, Cross, L1, Up
        self.assertEqual(frame[8:14], bytes([11, 22, 33, 44, 128, 128]))
        self.assertEqual(struct.unpack_from("<I", frame, 36)[0], 55 | (66 << 16))
        clock[0] += 250000
        self.assertEqual(invoke("copy_input", [handler["source"], destination]), 0xFFFFFFFF)
        frame = bytes(cpu.mem_read(destination, 48))
        self.assertEqual(struct.unpack_from("<II", frame), (0x12345678, 0))
        self.assertEqual(frame[8:12], bytes([128] * 4))
        previous = len(calls)
        invoke("gobt_sink_maintain", [])
        self.assertEqual(len(calls), previous)  # stale input cannot reclaim port
        invoke("pspgobtDisconnect", [])
        clock[0] += 100000
        invoke("gobt_sink_maintain", [])
        self.assertEqual(len(calls), previous)

    def test_complete_full_report_reaches_ds3_transfer_handler(self):
        self.exercise()

    def test_basic_report_reaches_ds3_transfer_handler(self):
        self.exercise(simple=True)

    def test_corrupt_full_report_does_not_register_or_inject_input(self):
        self.exercise(corrupt=True)

    def test_new_instance_selects_cached_controller_without_did(self):
        self.exercise(prepared=False)

    def test_reconnect_reclaims_transfer_handler_and_passthrough(self):
        self.exercise(reconnect=True)

    def test_delayed_firmware_reset_restores_handler_and_passthrough(self):
        self.exercise(delayed_reset=True)

    def receive_error(self, status):
        cpu = machine()
        cpu.mem_map(0x08200000, 0x10000)
        context, entry, data, callback = 0x88201000, 0x88202000, 0x88203000, 0x0A700100
        physical = lambda p: p & 0x1FFFFFFF
        cpu.mem_write(physical(context), struct.pack("<IHH", 2, 1, 1))
        cpu.mem_write(physical(context + 40), struct.pack("<I", entry))
        cpu.mem_write(physical(entry + 8), struct.pack("<I", data))
        cpu.mem_write(physical(data), b"\xCC" * 128)
        for name, value in (("contexts", context), ("wide_native", data), ("wide_active", 1),
                            ("reads_ready", 1), ("generations", 7), ("original_receive", callback)):
            word(cpu, name, value)
        calls = []

        def observe(m, address, _, __):
            returns = {SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                       SYMBOLS["sceKernelCpuResumeIntr"]: 0,
                       SYMBOLS["sceKernelGetSystemTimeWide"]: 1000}
            if address == callback:
                expected_status = 0xFFFFFFFF if status == 129 else status
                args = [m.reg_read(r) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2)]
                self.assertEqual(args, [data, expected_status, context])
                calls.append(args)
                returns[address] = 77
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_V1, 0)
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        for r, value in zip((reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1, reg.UC_MIPS_REG_A2),
                             (SYMBOLS["receive_buffers"], status, context)):
            cpu.reg_write(r, value)
        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.emu_start(SYMBOLS["receive_wide"], 0x0A700F00, count=10000)
        self.assertEqual(len(calls), 1)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_V0), 77)
        self.assertEqual(bytes(cpu.mem_read(physical(data), 128)), b"\xCC" * 128)
        for name, value in (("wide_active", 0), ("reads_ready", 0), ("generations", 8)):
            self.assertEqual(struct.unpack("<I", cpu.mem_read(SYMBOLS[name], 4))[0], value)
        self.assertEqual(struct.unpack("<II", cpu.mem_read(SYMBOLS["queue"] + 16, 8)), (2, status))

    def test_receive_error_preserves_transport_status_and_stops_initialization(self):
        self.receive_error(0x80300009)

    def test_oversized_completion_never_copies_past_native_buffer(self):
        self.receive_error(129)

    def test_zero_length_completion_closes_read_without_copying(self):
        self.receive_error(0)


if __name__ == "__main__":
    unittest.main()
