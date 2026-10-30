"""Read the release version from the PRX module declaration."""
import argparse
from pathlib import Path
import re


def project_version():
    source = Path(__file__).resolve().parents[1] / "src/plugin.c"
    match = re.search(
        r'PSP_MODULE_INFO\("PSPGoBT_Probe",\s*[^,\n]+,\s*(\d+),\s*(\d+)\s*\);',
        source.read_text(),
    )
    if not match or any(int(part) > 255 for part in match.groups()):
        raise ValueError("Cannot read a valid PRX major/minor version from src/plugin.c")
    return ".".join(str(int(part)) for part in match.groups())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", help="Require this tag to match the PRX version (e.g. v1.0)")
    args = parser.parse_args()
    version = project_version()
    if args.tag is not None and args.tag != f"v{version}":
        parser.error(f"tag {args.tag!r} does not match PRX version v{version}")
    print(version)
