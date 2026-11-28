#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# tree_gate.py --tree src/host/mister/tree --pin src/host/mister/sys.pin
#
# The hand-written half of the MiSTer tree must stay what MiSTer's
# maintainers can rebuild with Quartus 17.0.2 and a fresh sys/:
#
# - RP6502.qsf keeps Template.qsf's LAST_QUARTUS_VERSION line, which
#   Quartus rewrites in any project it opens, so a tree that went through
#   the IDE shows here;
# - no file uses TIMING_ANALYZER_, the name later Quartus versions give
#   the TIMEQUEST_ settings, which 17.0.2 rejects;
# - there is no sys/, which comes only from the pin, unedited;
# - sys.pin names a url, a commit and a tree, once each.

import argparse
import re
import sys
from pathlib import Path

VERSION_LINE = ('set_global_assignment -name LAST_QUARTUS_VERSION'
                ' "17.0.2 Standard Edition"')
PIN = {
    "url": re.compile(r"https://\S+"),
    "commit": re.compile(r"[0-9a-f]{40}"),
    "tree": re.compile(r"[0-9a-f]{40}"),
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", required=True, type=Path)
    ap.add_argument("--pin", required=True, type=Path)
    a = ap.parse_args()

    bad = []
    qsf = a.tree / "RP6502.qsf"
    if VERSION_LINE not in qsf.read_text().splitlines():
        bad.append(f"{qsf}: no line {VERSION_LINE}")
    for path in sorted(p for p in a.tree.rglob("*") if p.is_file()):
        if b"TIMING_ANALYZER_" in path.read_bytes():
            bad.append(f"{path}: TIMING_ANALYZER_ is not 17.0.2's name")
    if (a.tree / "sys").exists():
        bad.append(f"{a.tree / 'sys'}: sys/ comes from sys.pin, not the"
                   " tree")

    seen: dict[str, str] = {}
    for lineno, line in enumerate(a.pin.read_text().splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        key, _, value = line.partition(" ")
        if key not in PIN or not PIN[key].fullmatch(value):
            bad.append(f"{a.pin}:{lineno}: not 'url https://...', 'commit"
                       " <40 hex>' or 'tree <40 hex>'")
        elif key in seen:
            bad.append(f"{a.pin}:{lineno}: a second {key}")
        else:
            seen[key] = value
    for key in PIN.keys() - seen.keys():
        bad.append(f"{a.pin}: no {key}")

    for line in bad:
        print(f"tree_gate: {line}", file=sys.stderr)
    if bad:
        return 1
    print("tree_gate: the tree is Template-shaped and sys.pin is whole")
    return 0


if __name__ == "__main__":
    sys.exit(main())
