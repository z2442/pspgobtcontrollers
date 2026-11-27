"""Read, choose, or stamp the release version in the PRX module declarations."""
import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
MODULES = {
    ROOT / "src/plugin.c": "PSPGoBT_Probe",
    ROOT / "ui/mapping_ui.c": "PSPGoBT_Mapping",
}


def module_pattern(name):
    return re.compile(
        rf'(PSP_MODULE_INFO\("{name}",\s*[^,\n]+,\s*)(\d+)(,\s*)(\d+)(\s*\);)'
    )


def parse_version(text):
    match = re.fullmatch(r"(\d+)\.(\d+)", text)
    if not match or any(int(part) > 255 for part in match.groups()):
        raise ValueError(f"{text!r} is not a valid PRX major.minor version (each part 0-255)")
    return tuple(int(part) for part in match.groups())


def project_version():
    source = ROOT / "src/plugin.c"
    match = module_pattern("PSPGoBT_Probe").search(source.read_text())
    if not match:
        raise ValueError("Cannot read a valid PRX major/minor version from src/plugin.c")
    major, minor = int(match.group(2)), int(match.group(4))
    parse_version(f"{major}.{minor}")
    return f"{major}.{minor}"


def next_major(tags, commit=None):
    """Return the next major release after the source version and every vX.Y tag.

    Each line of `tags` is `refs/tags/<name> [<sha>]`. A vX.Y tag already on
    `commit` is reused so a re-run publishes to the same release. Tags that are
    not plain `vX.Y` (such as old `v1.0-build.N` builds) are ignored.
    """
    majors = [parse_version(project_version())[0] - 1]
    for line in tags:
        tag, _, sha = line.strip().partition(" ")
        tag = tag.removeprefix("refs/tags/")
        match = re.fullmatch(r"v(\d+)\.(\d+)", tag)
        if not match:
            continue
        if commit and sha.strip() == commit:
            return f"{int(match.group(1))}.{int(match.group(2))}"
        majors.append(int(match.group(1)))
    version = f"{max(majors) + 1}.0"
    parse_version(version)
    return version


def stamp(version):
    major, minor = parse_version(version)
    for path, name in MODULES.items():
        text, count = module_pattern(name).subn(
            lambda m: f"{m.group(1)}{major}{m.group(3)}{minor}{m.group(5)}", path.read_text()
        )
        if count != 1:
            raise ValueError(f"Cannot find the {name} module version in {path}")
        path.write_text(text)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--tag", help="Require this tag to match the PRX version (e.g. v1.0)")
    action.add_argument("--next-major", action="store_true",
                        help="Print the next major version, reading `ref [sha]` tag lines from stdin")
    parser.add_argument("--commit", help="With --next-major, reuse a vX.Y tag already on this commit")
    action.add_argument("--stamp", metavar="X.Y", help="Write this version into both PRX modules")
    args = parser.parse_args()
    try:
        if args.next_major:
            print(next_major(sys.stdin, args.commit))
            raise SystemExit(0)
        if args.stamp is not None:
            stamp(args.stamp)
        version = project_version()
    except ValueError as error:
        parser.error(str(error))
    if args.tag is not None and args.tag != f"v{version}":
        parser.error(f"tag {args.tag!r} does not match PRX version v{version}")
    print(version)
