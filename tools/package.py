"""Package a release/debug PRX with only whitelisted documentation."""
import argparse
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import shutil
from version import project_version
from psp_exports import function_exports, function_nid

root = Path(__file__).resolve().parents[1] / "dist"
parser = argparse.ArgumentParser()
parser.add_argument("--debug", type=int, choices=(0, 1), default=0)
parser.add_argument("--session-trace", type=int, choices=(0, 1), default=0)
parser.add_argument("--mapping-ui", type=int, choices=(0, 1), default=1)
args = parser.parse_args()
debug, session_trace = args.debug, args.session_trace
files = [
    "README.md", "CHANGELOG.md", "LICENSE", "THIRD_PARTY_NOTICES.md", "docs/research.md", "docs/capture-analysis.md",
    "docs/capture-v02-analysis.md",
    "docs/capture-v03-analysis.md",
    "docs/capture-v04-analysis.md",
    "docs/capture-v05-analysis.md",
    "docs/capture-v08-analysis.md",
    "docs/capture-v09-analysis.md",
    "docs/capture-v10-analysis.md",
    "docs/capture-v11-analysis.md",
    "docs/capture-v12-analysis.md",
    "docs/reconnect-v015.md",
    "docs/reconnect-debug-v016.md",
    "docs/reconnect-v017.md",
    "docs/reconnect-v018.md",
    "docs/reconnect-v019.md",
    "docs/handoff-v020.md",
    "docs/session-v021.md",
    "docs/manager-session-v022.md",
    "docs/session-v023.md",
    "docs/generic-hid-v024.md",
    "docs/hid-mode-v025.md",
    "docs/transport-v026.md",
    "docs/connect-v027.md",
    "docs/performance-v028.md",
    "docs/input-latency.md",
    "docs/reconnect-input.md",
    "docs/switch-pro.md",
    "seplugins/pspgobt/pspgobt_probe.prx", "seplugins/pspgobt/mapping_ui.prx", "seplugins/pspgobt/PLUGINS.TXT.example",
]
variant = "debug" if debug else "session-trace" if session_trace else "release"
if not args.mapping_ui:
    variant += "-no-mapping"
    files.remove("seplugins/pspgobt/mapping_ui.prx")  # Never loaded by this build.
binary = (root / "seplugins/pspgobt/pspgobt_probe.prx").read_bytes()
if (b"mapping-ui:enabled" in binary) != bool(args.mapping_ui) or (b"mapping-ui:disabled" in binary) != (not args.mapping_ui):
    raise SystemExit("Refusing to package: mapping UI build profile mismatch")
if not any(library == "PSPGoBT_trace" and nid == function_nid("pspgobtMappingService") and flags & 0x4000
           for library, nid, _, flags in function_exports(binary)):
    raise SystemExit("Refusing to package: controller mapping service is not exported as a syscall")
version = project_version()
major, minor = (int(part) for part in version.split("."))
module = binary.find(b"PSPGoBT_Probe\0")
if module < 2 or binary[module - 2:module] != bytes([minor, major]):
    raise SystemExit("Refusing to package: PRX module version does not match src/plugin.c")
for marker in (b"probe.log", b".text.bin"):
    if (marker in binary) != bool(debug):
        raise SystemExit(f"Refusing to package: PRX diagnostics do not match {variant} profile")
if (b"session.log" in binary) != bool(session_trace):
    raise SystemExit("Refusing to package: session trace profile mismatch")
release = root / f"pspgobt-{variant}-v{version}.zip"
with ZipFile(release, "w", compression=ZIP_DEFLATED) as archive:
    for name in files:
        archive.write(root / name, name)
shutil.copyfile(release, root / f"pspgobt-{variant}.zip")
if debug:
    shutil.copyfile(release, root / "pspgobt-bringup.zip")
print(f"Packaged: {release}")
