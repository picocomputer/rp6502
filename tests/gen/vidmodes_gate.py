#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The video-mode corpus against its readers.
#
# tests/roms/vidmodes.py writes forty-seven ROMs. tests/cpu/vga/test_modes.c
# boots nearly all of them on whichever machine its tree builds and holds each
# frame to the CRC in its case; the one fixture the two machines disagree
# about by design is asserted in tests/rtl/vga, where the machine that owns
# that behaviour is.
#
# The suites name every file by hand, and they have to: the case names are not
# the file names, because they name what they prove. Generating the lists
# would take that away.
#
# So the names stay written by hand and this says whether they are all read by
# something. A fixture added to the generator and to no suite is not a failure
# anywhere — it is forty-six of forty-seven, quietly, forever.

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

    # Covered by some suite, not by every suite. A fixture the machines
    # disagree about on purpose is asserted where the machine that owns it
    # is, so demanding each reader take the whole corpus would fail for the
    # one case that cannot be shared. What must not happen is a generated
    # fixture nobody boots.
    seen = set()
    per = []
    for suite in a.suite:
        mine = read(suite, known)
        per.append(f"{Path(suite).name} {len(mine)}")
        seen |= mine
    missing = sorted(set(known) - seen)
    if missing:
        print(f"{len(known) - len(missing)} of {len(known)} fixtures are read "
              f"by a suite, missing: {', '.join(missing)}", file=sys.stderr)
        return 1
    print(f"vidmodes: {len(known)} fixtures, all read — {', '.join(per)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
