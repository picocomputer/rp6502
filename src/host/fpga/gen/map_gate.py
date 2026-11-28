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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("map_rpt")
    ap.add_argument("--max-registers", type=int, default=MAX_REGISTERS)
    ap.add_argument("--max-m10k", type=int, required=True,
                    help="the device's M10K blocks")
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

    print("map ok: registers and memory inside the rails")
    return 0


if __name__ == "__main__":
    sys.exit(main())
