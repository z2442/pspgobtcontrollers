"""Check diagnostic exclusion and essential module setup in each PSP build."""
import argparse
from pathlib import Path
import struct
import subprocess
import unittest

from unicorn import UC_HOOK_CODE
from unicorn import mips_const as reg
from test_bridge_execution import machine, SYMBOLS, word

parser = argparse.ArgumentParser()
parser.add_argument("--debug", type=int, choices=(0, 1), required=True)
parser.add_argument("--session-trace", type=int, choices=(0, 1), default=0)
args = parser.parse_args()
DEBUG, TRACE = args.debug, args.session_trace


class BuildProfile(unittest.TestCase):
    def test_startup_observes_modules_before_inventory_and_always_starts_retry_worker(self):
        cpu, calls = machine(), []

        def observe(m, address, _, __):
            returns = {SYMBOLS["sceKernelGetModel"]: 4,
                       SYMBOLS["sceKernelDevkitVersion"]: 0x06060010,
                       SYMBOLS["sceIoOpen"]: 0xFFFFFFFF,
                       SYMBOLS["gobt_log"]: 0}
            if address == SYMBOLS.get("gobt_session_trace_start"):
                calls.append("start-trace")
                returns[address] = 0
            if "sceIoMkdir" in SYMBOLS:
                returns[SYMBOLS["sceIoMkdir"]] = 0
            if address == SYMBOLS["sctrlHENSetStartModuleHandler"]:
                calls.append("observe-modules")
                returns[address] = 0
            if address == SYMBOLS["sceKernelGetModuleIdList"]:
                calls.append("inventory")
                m.mem_write(m.reg_read(reg.UC_MIPS_REG_A2), bytes(4))
                returns[address] = 0
            if address == SYMBOLS["gobt_start_input_bridge"]:
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), 0x06060010)
                calls.append("start-worker")
                returns[address] = 0
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.reg_write(reg.UC_MIPS_REG_A0, 0)
        cpu.reg_write(reg.UC_MIPS_REG_A1, 0)
        cpu.emu_start(SYMBOLS["module_start"], 0x0A700F00, count=20000)
        self.assertEqual(calls, (["start-trace"] if TRACE else []) + ["observe-modules", "inventory", "start-worker"])

    def test_release_excludes_diagnostic_strings_and_write_imports(self):
        binary = Path("pspgobt_probe.prx").read_bytes()
        names = {line.split()[-1] for line in subprocess.check_output(
            ["psp-nm", "pspgobt_probe.elf"], text=True).splitlines() if line.split()}
        for name in ("gobt_log", "sceIoMkdir", "prnt", "sprintf"):
            self.assertEqual(name in names, bool(DEBUG), name)
        for marker in (b"probe.log", b".text.bin", b"bt-input ", b"bt-session ", b"stored-record"):
            self.assertEqual(marker in binary, bool(DEBUG), marker)
        # Release writes only its small controller configuration through the
        # dedicated cache path; diagnostic strings/writers remain excluded.
        self.assertIn(b"controllers.cfg", binary)
        self.assertEqual(b"session.log" in binary, bool(TRACE))
        self.assertEqual("trace_ring" in names, bool(TRACE))
        self.assertEqual("gobt_session_trace" in names, bool(TRACE))

    def setup_module(self, name, expected):
        cpu = machine()
        module = 0x0A600000
        record = bytearray(256)
        record[8:8 + len(name)] = name.encode()
        struct.pack_into("<I", record, 44, 7)
        cpu.mem_write(module, bytes(record))
        word(cpu, "firmware", 0x06060010)
        cpu.reg_write(reg.UC_MIPS_REG_A0, module)
        calls = []
        installs = {SYMBOLS[fn]: fn for fn in (
            "gobt_install_pair_command_trace", "gobt_install_input_bridge",
            "gobt_trace_device_info", "gobt_install_hid_route", "gobt_install_pair_route")}

        def cstring(m, pointer):
            return bytes(m.mem_read(pointer, 64)).split(b"\0", 1)[0]

        def observe(m, address, _, __):
            returns = {SYMBOLS["gobt_capture_module"]: 1, SYMBOLS["gobt_log"]: 0,
                       SYMBOLS["sceKernelCpuSuspendIntr"]: 1,
                       SYMBOLS["sceKernelCpuResumeIntr"]: 0}
            if address in installs:
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A0), module)
                self.assertEqual(m.reg_read(reg.UC_MIPS_REG_A1), 0x06060010)
                calls.append(installs[address])
                returns[address] = 0
            if address == SYMBOLS["strcmp"]:
                left, right = [cstring(m, m.reg_read(r)) for r in (reg.UC_MIPS_REG_A0, reg.UC_MIPS_REG_A1)]
                returns[address] = 0 if left == right else 1
            if address in returns:
                m.reg_write(reg.UC_MIPS_REG_V0, returns[address])
                m.reg_write(reg.UC_MIPS_REG_PC, m.reg_read(reg.UC_MIPS_REG_RA))

        cpu.hook_add(UC_HOOK_CODE, observe)
        cpu.emu_start(SYMBOLS["inspect_module"], 0x0A700F00, count=20000)
        self.assertEqual(calls, expected)
        self.assertEqual(cpu.reg_read(reg.UC_MIPS_REG_SP), 0x0A60F000)

    def test_bluetooth_manager_still_installs_input_bridge(self):
        expected = ["gobt_install_pair_command_trace"] if DEBUG else []
        self.setup_module("sceBSMan", expected + ["gobt_install_input_bridge"])

    def test_xmb_still_installs_connection_pairing_and_routing_hooks(self):
        self.setup_module("bluetooth_plugin_module", ["gobt_trace_device_info",
                          "gobt_install_hid_route", "gobt_install_pair_route"])


if __name__ == "__main__":
    unittest.main(argv=[__file__])
