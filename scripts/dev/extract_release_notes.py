#!/usr/bin/env python3
"""Print only the changelog section for one version.

Release notes describe the version being published. The full CHANGELOG-cn.md
stays in the repo (and feeds the on-device viewer), but pasting it wholesale
into a GitHub release makes every release list the entire version history.
"""
import pathlib
import re
import sys

VERSION_HEADING = re.compile(r"^##\s+v?(\S+)\s*$")
ANY_H2 = re.compile(r"^##\s+\S")


def section_for(lines, version):
    start = None
    for i, line in enumerate(lines):
        match = VERSION_HEADING.match(line)
        if match and match.group(1) == version:
            start = i
            break
    if start is None:
        return None
    end = len(lines)
    for j in range(start + 1, len(lines)):
        if ANY_H2.match(lines[j]):
            end = j
            break
    return "\n".join(lines[start:end]).strip()


def main(path, version):
    text = pathlib.Path(path).read_text(encoding="utf-8")
    version = version.strip().lstrip("vV")
    section = section_for(text.splitlines(), version)
    if not section:
        print(f"{path}: no changelog section for version {version}", file=sys.stderr)
        return 1
    sys.stdout.write(section + "\n")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: extract_release_notes.py <changelog.md> <version>", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
