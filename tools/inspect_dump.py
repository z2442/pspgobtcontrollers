#!/usr/bin/env python3
"""Inspect a PSPGoBT text capture without executing firmware code."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def inspect(path):
    data = Path(path).read_bytes()
    result = {
        "file": Path(path).name,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }
    if "bluetooth_plugin" in Path(path).name:
        sites = ((0x13D40, 0x240E0005, 0x240EFFFF),
                 (0x13D6C, 0x104E0003, 0x144E0003))
        result["filter_sites"] = []
        states = []
        for offset, original, patched in sites:
            word = struct.unpack_from("<I", data, offset)[0] if len(data) >= offset + 4 else None
            state = ("original" if word == original else
                     "patched" if word == patched else "mismatch")
            states.append(state)
            result["filter_sites"].append({
                "offset": f"0x{offset:08x}",
                "word": None if word is None else f"0x{word:08x}",
                "state": state,
            })
        result["filter_patch_candidate"] = states == ["original", "original"]
        result["note"] = "Filter match does not establish pairing or HID transport support."
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path, nargs="+")
    args = parser.parse_args()
    try:
        print(json.dumps([inspect(path) for path in args.captures], indent=2))
    except OSError as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
