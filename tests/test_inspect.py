import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import sys

SPEC = importlib.util.spec_from_file_location("inspect_dump", Path(__file__).resolve().parents[1] / "tools/inspect_dump.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from analyze_probe import analyze


class DumpTests(unittest.TestCase):
    def test_filter_boundaries(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "bluetooth_plugin_module-1.text.bin"
            path.write_bytes(b"\0" * 32)
            self.assertFalse(MODULE.inspect(path)["filter_patch_candidate"])
            data = bytearray(0x13D70)
            struct.pack_into("<I", data, 0x13D40, 0x240E0005)
            struct.pack_into("<I", data, 0x13D6C, 0x104E0003)
            path.write_bytes(data)
            self.assertTrue(MODULE.inspect(path)["filter_patch_candidate"])
            struct.pack_into("<I", data, 0x13D6C, 0x144E0003)
            path.write_bytes(data)
            self.assertFalse(MODULE.inspect(path)["filter_patch_candidate"])

    def test_capture_inventory(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "probe.log").write_text(
                "PSPGoBT development probe 0.1 firmware=06060010 model=4\n"
                "loaded sceBSMan\nloaded sceController_Service\n"
                "module sceHIDSvc uid=1 base=8822F800 text=00000020\n"
                "import sceInput_driver nid=12345678 stub=88230000\n"
                "module sceVshNetconf_bt_Module uid=2 base=0A640F00 text=00000020\n"
                "device-info event=17 device=00000001 result=00000000 record80=1 field06=0044 field48=0402\n"
                "device-info event=18 device=00000001 result=00000000 record80=1 field48=0402\n"
                "bt-api end op=register-scan event=1 result=802F0135\n"
            )
            (root / "sceHIDSvc-1.text.bin").write_bytes(bytes(32))
            report = analyze(root)
            self.assertEqual(report["runs"][0]["firmware"], "0x06060010")
            self.assertTrue(report["modules"][0]["size_matches_log"])
            self.assertEqual(report["modules"][0]["imports"], ["sceInput_driver"])
            self.assertEqual(report["missing_files"], ["sceVshNetconf_bt_Module-2.text.bin"])
            self.assertEqual(report["providers_loaded_but_not_captured"], ["sceBSMan", "sceController_Service"])
            self.assertEqual(len(report["nonzero_device_fields"]), 2)
            self.assertEqual(report["nonzero_device_fields"][0]["field48"], 0x402)
            self.assertEqual(report["api_results"], [{"op": "register-scan", "event": 1, "result": 0x802f0135}])


if __name__ == "__main__":
    unittest.main()
