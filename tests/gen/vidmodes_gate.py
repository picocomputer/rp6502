#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The video-mode corpus against its readers.
#
# tests/roms/vidmodes.py writes forty-seven ROMs; tests/emu/vid/test_vidmodes.c
# boots each one and asserts a settled picture, and tests/rtl/vid/test_modes.cpp
# runs the same files on both machines and demands pixel equality. Both name
# every file by hand, and they have to: the case names are not the file names,
# because test_modes.cpp names its cases for what they prove. Generating the
# lists would take that away.
#
# So the names stay written by hand and this says whether they are all there.
# A fixture added to the generator and to only one of the two suites is not a
# failure anywhere — it is forty-six of forty-seven, quietly, forever.

import argparse
import re
import sys
from pathlib import Path


def names(manifest):
    out = []
    for line in Path(manifest).read_text().splitlines():
        if line.strip():
            out.append(line.split()[0])
    return out


def read(suite, known):
    """Every manifest name a suite mentions, however it spells the reference:
    run_case("x"), ROMS_DIR "/x.rp6502", or whatever the next one is.

    The string literals are collected whole first. Looking for a name inside
    one with a single pattern lets the run of not-a-quote start at a closing
    quote and cross the code to the next mention, which reads every fixture as
    covered no matter what the suite says."""
    literals = re.findall(r'"((?:[^"\\\n]|\\.)*)"', Path(suite).read_text())
    seen = set()
    for lit in literals:
        for name in known:
            if re.search(r"(?<![0-9A-Za-z_])" + re.escape(name) +
                         r"(?![0-9A-Za-z_])", lit):
                seen.add(name)
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--suite", action="append", required=True)
    a = ap.parse_args()

    known = names(a.manifest)
    if not known:
        print("vidmodes_gate: the manifest is empty", file=sys.stderr)
        return 1

    bad = 0
    for suite in a.suite:
        missing = sorted(set(known) - read(suite, known))
        if missing:
            bad = 1
            print(f"{Path(suite).name} reads {len(known) - len(missing)} of "
                  f"{len(known)} fixtures, missing: {', '.join(missing)}",
                  file=sys.stderr)
    if not bad:
        print(f"vidmodes: {len(known)} fixtures, {len(a.suite)} readers, all covered")
    return bad


if __name__ == "__main__":
    raise SystemExit(main())
