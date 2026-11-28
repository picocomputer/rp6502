#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import re
import sys
from pathlib import Path

# The design maps to about 15,000 registers, and this limit is twice that.
MAX_REGISTERS = 30000

# A Cyclone V M10K block holds 10,240 bits. Synthesis reports the bits its
# memories need, not the blocks: how they pack into blocks is the fitter's,
# so this rail catches only memories that could not fit at any packing.
M10K_BITS = 10240

# A project that is meant to build from its own folder alone, as a MiSTer
# tree must for whoever takes it, can still read a file from anywhere: a
# path in a .qip or a search path that only this machine has. Synthesis
# lists every file it read, so each one the project named or Quartus found
# must lie under the root. The rest come with Quartus: its megafunctions,
# and the files it generates into db/.
EXEMPT = re.compile(r"Megafunction|Auto-Generated")


def outside(text: str, root: Path) -> list[str] | None:
    lines = text.splitlines()
    try:
        start = next(i for i, ln in enumerate(lines)
                     if ln.startswith("; Analysis & Synthesis Source Files Read"))
    except StopIteration:
        return None
    found = []
    for ln in lines[start + 3:]:
        if not ln.startswith(";"):
            if ln.startswith("+") and found:
                break
            continue
        cells = [c.strip() for c in ln.strip(";").split(";")]
        if len(cells) < 4 or cells[2] == "File Type":
            continue
        found.append(cells)
    bad = []
    for name, _, kind, path, *_ in found:
        if EXEMPT.match(kind):
            continue
        try:
            Path(path).resolve().relative_to(root)
        except ValueError:
            bad.append(f"{path} ({kind}) is outside {root}")
    return bad if found else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("map_rpt")
    ap.add_argument("--max-registers", type=int, default=MAX_REGISTERS)
    ap.add_argument("--max-m10k", type=int, required=True,
                    help="the device's M10K blocks")
    ap.add_argument("--root", type=Path,
                    help="the folder every source file must lie under")
    a = ap.parse_args()

    text = Path(a.map_rpt).read_text(errors="replace")
    bad = []

    m = re.search(r"^; Total registers\s*;\s*([\d,]+)", text, re.M)
    if not m:
        bad.append("no Total registers line in the map report")
    else:
        regs = int(m.group(1).replace(",", ""))
        if regs > a.max_registers:
            bad.append(
                f"{regs} registers (rail {a.max_registers}): a memory "
                f"has fallen out of BRAM inference -- look for an array "
                f"with two write ports or a read the RAM cannot serve")

    m = re.search(r"^; Total block memory bits\s*;\s*([\d,]+)", text, re.M)
    if not m:
        bad.append("no Total block memory bits line in the map report")
    else:
        bits = int(m.group(1).replace(",", ""))
        if bits > a.max_m10k * M10K_BITS:
            bad.append(f"{bits} block memory bits over the device's "
                       f"{a.max_m10k} M10K blocks")

    if bad:
        print("map_gate: the synthesis result cannot fit; "
              "not paying for a fit to prove it")
        for line in bad:
            print(f"  {line}")
        return 1

    if a.root:
        root = a.root.resolve()
        far = outside(text, root)
        if far is None:
            print("map_gate: no Source Files Read table in the map report")
            return 1
        if far:
            print("map_gate: the project reads files from outside its "
                  "folder, which a copy of the folder would not have")
            for line in far:
                print(f"  {line}")
            return 1

    print("map ok: registers and memory inside the rails"
          + (", every source under the root" if a.root else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
