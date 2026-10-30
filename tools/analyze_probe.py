#!/usr/bin/env python3
"""Match a probe log to captures and optionally disassemble each relocated image."""
import argparse
import json
from pathlib import Path
import re
import subprocess
from inspect_dump import inspect

MODULE = re.compile(r"^module ([A-Za-z0-9_]{1,27}) uid=([0-9A-Fa-f]+) base=([0-9A-Fa-f]+) text=([0-9A-Fa-f]+)$")
INFO_EVENT = re.compile(
    r"^device-info event=(\d+) device=([0-9A-Fa-f]+) result=([0-9A-Fa-f]+) "
    r"record80=(\d+)(?: field06=[0-9A-Fa-f]+)? field48=([0-9A-Fa-f]+)$"
)
API_RESULT = re.compile(r"^bt-api end op=([a-z-]+) event=(\d+) result=([0-9A-Fa-f]+)$")


def analyze(folder):
    folder = Path(folder)
    text = (folder / "probe.log").read_text()
    firmware = re.findall(r"probe ([\d.]+) firmware=([0-9A-Fa-f]+) model=(\d+)", text)
    loaded = set()
    modules = []
    info_records, api_results = [], []
    current = None
    for line in text.splitlines():
        info = INFO_EVENT.fullmatch(line)
        api = API_RESULT.fullmatch(line)
        if info:
            event, device, result, record80, field48 = info.groups()
            info_records.append({
                "event": int(event), "device": int(device, 16),
                "result": int(result, 16), "record80": bool(int(record80)),
                "field48": int(field48, 16),
            })
        if api:
            op, event, result = api.groups()
            api_results.append({"op": op, "event": int(event), "result": int(result, 16)})
        match = MODULE.fullmatch(line)
        if match:
            name, uid, base, size = match.groups()
            path = folder / f"{name}-{uid}.text.bin"
            current = {
                "name": name, "uid": uid, "base": f"0x{base}",
                "expected_bytes": int(size, 16), "file": path.name,
                "present": path.is_file(), "imports": [],
            }
            if path.is_file():
                current["image"] = inspect(path)
                current["size_matches_log"] = path.stat().st_size == int(size, 16)
            modules.append(current)
        elif line.startswith("loaded "):
            loaded.add(line[7:])
        elif current is not None and line.startswith("import "):
            library = line.split()[1]
            if library not in current["imports"]:
                current["imports"].append(library)
    captured_names = {module["name"] for module in modules if module["present"]}
    providers = ("sceBSMan", "sceUsbBSMCDC_Driver", "sceController_Service")
    return {
        "runs": [{"probe": version, "firmware": f"0x{fw}", "model": int(model)}
                 for version, fw, model in firmware],
        "modules": modules,
        "missing_files": [module["file"] for module in modules if not module["present"]],
        "providers_loaded_but_not_captured": [name for name in providers
                                              if name in loaded and name not in captured_names],
        "input_test_results": [line for line in text.splitlines()
                               if line.startswith("local native DS3 input test start result=")],
        "device_info_events": [line for line in text.splitlines()
                               if line.startswith("device-info event=")],
        "device_info_records": info_records,
        "nonzero_device_fields": [record for record in info_records
                                  if record["result"] == 0 and record["record80"] and record["field48"]],
        "api_results": api_results,
        "filter_results": [line for line in text.splitlines()
                           if line.startswith("discovery filter")],
    }


def disassemble(folder, report, output):
    if len(report["runs"]) != 1:
        raise ValueError("Disassembly requires a log from one boot to avoid ambiguous relocation bases")
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    for module in report["modules"]:
        if not module["present"] or not module["size_matches_log"]:
            continue
        # One log can contain module names supplied by the target; the filename
        # must remain a single path component, and shell interpretation is never used.
        filename = module["file"]
        if Path(filename).name != filename:
            raise ValueError("capture filename must be a single path component")
        with (output / (filename + ".asm")).open("w") as stream:
            subprocess.run([
                "psp-objdump", "-D", "-b", "binary", "-m", "mips:allegrex", "-EL",
                "--adjust-vma=" + module["base"], str(Path(folder) / filename),
            ], stdout=stream, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--disassemble", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = analyze(args.folder)
    if args.disassemble:
        disassemble(args.folder, report, args.disassemble)
    result = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(result)
    else:
        print(result, end="")


if __name__ == "__main__":
    main()
