#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# sys_sdc_gate.py --sdc sys/sys_top.sdc --rtl FILE ... [--rtl-dir DIR ...]
#
# sys/sys_top.sdc cuts and relaxes paths by bare name patterns, such as
# -to {cfg[*]} or -to {*_osd|v_cnt*}, written for sys_top's own registers.
# Quartus would apply them as well to any of this core's registers whose
# name they match, and say nothing. It matches a pattern against a node's
# whole name, which for this core starts with emu| (emu:emu| with the
# entities), and its * crosses the | between levels, so on the trial
# Template build cfg[*] matches sys_top's eight cfg registers and
# act_cnt* matches nothing, while *act_cnt* and emu|act_cnt* match
# Template.sv's act_cnt. A pattern reaches this core only through a
# leading *, or a first level that can match emu.
#
# The gate does not elaborate the design. It takes every word of the RTL
# as a possible instance or register name, and fails when each level of a
# pattern that reaches the core matches one: the first level, after a
# leading *, the end of a name; every other level a whole name.

import argparse
import re
import sys
from fnmatch import fnmatchcase
from pathlib import Path

BARE = re.compile(r"-(?:from|to|through)\s+\{([^{}]*)\}")
WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_$]*")
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def levels(pattern: str) -> list[str]:
    """The pattern's levels, each without its bit index or entity."""
    return [re.sub(r"\[[^\]]*\]", "", seg.split(":")[0])
            for seg in pattern.split("|")]


def hits(pattern: str, names: set[str]) -> list[list[str]] | None:
    segs = levels(pattern)
    if pattern.startswith("*"):
        first = "*" + segs[0][1:]
    elif fnmatchcase("emu", segs[0]):
        segs = segs[1:]
        if not segs:
            return None
        first = segs[0]
    else:
        return None
    found = [sorted(n for n in names if fnmatchcase(n, first))]
    for seg in segs[1:]:
        found.append(sorted(n for n in names if fnmatchcase(n, seg)))
    return found if all(found) else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdc", required=True, type=Path)
    ap.add_argument("--rtl", action="append", default=[], type=Path)
    ap.add_argument("--rtl-dir", action="append", default=[], type=Path)
    a = ap.parse_args()

    sources = list(a.rtl)
    for d in a.rtl_dir:
        sources += sorted(d.rglob("*.sv")) + sorted(d.rglob("*.v"))
    names: set[str] = set()
    for path in sources:
        if path.exists():
            names |= set(WORD.findall(
                COMMENT.sub(" ", path.read_text(errors="replace"))))
    if not names:
        sys.exit("sys_sdc_gate: no RTL was readable")

    text = a.sdc.read_text(errors="replace")
    text = re.sub(r"(?m)^\s*#.*$", "", text)
    patterns: dict[str, int] = {}
    for m in BARE.finditer(text):
        lineno = text.count("\n", 0, m.start()) + 1
        for p in m.group(1).split():
            if p != "\\":
                patterns.setdefault(p, lineno)
    if not patterns:
        sys.exit(f"sys_sdc_gate: no bare patterns in {a.sdc}; its format"
                 " moved")

    bad = 0
    for pattern, lineno in patterns.items():
        found = hits(pattern, names)
        if found:
            bad += 1
            print(f"{a.sdc}:{lineno}: {{{pattern}}} can match this core's "
                  + " | ".join(",".join(f[:4]) for f in found))
    if bad:
        print(f"\nsys_sdc_gate: {bad} of sys/'s patterns could constrain"
              " this core's registers; rename them")
        return 1
    print(f"sys_sdc_gate: none of {len(patterns)} patterns reaches the core")
    return 0


if __name__ == "__main__":
    sys.exit(main())
