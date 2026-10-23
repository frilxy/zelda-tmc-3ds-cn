#!/usr/bin/env python3
"""Fail the release build when CHANGELOG-cn.md cannot feed the on-device viewer.

The 3DS updater starts its changelog at the "## Changelog" heading and renders
through a font that only covers ASCII, dropping every other byte. A translation
accidentally pasted into that section would ship as blank lines, so the release
job refuses to publish in that case.
"""
import pathlib
import sys

HEADING = "## Changelog"


def main(path: str) -> int:
    text = pathlib.Path(path).read_text(encoding="utf-8")
    if HEADING not in text:
        print(f"{path}: missing a '{HEADING}' section", file=sys.stderr)
        return 1
    section = text.split(HEADING, 1)[1]
    bad = sorted({c for c in section if ord(c) > 0x7E})
    if bad:
        print(f"{path}: non-ASCII characters in the on-device changelog: {bad}", file=sys.stderr)
        return 1
    widest = max((len(line) for line in section.splitlines()), default=0)
    if widest > 84:
        print(f"{path}: changelog line is {widest} characters; the viewer wraps at 84", file=sys.stderr)
        return 1
    print(f"{path}: on-device changelog OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "CHANGELOG-cn.md"))
