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

# The Cyclone V 5CEBA4 has 308 M10K blocks.
MAX_M10K = 308


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("map_rpt")
    ap.add_argument("--max-registers", type=int, default=MAX_REGISTERS)
    ap.add_argument("--max-m10k", type=int, default=MAX_M10K)
    a = ap.parse_args()

    text = Path(a.map_rpt).read_text(errors="replace")
    bad = []

    m = re.search(r"Total registers\s*;\s*([\d,]+)", text)
    if not m:
        bad.append("no Total registers line in the map report")
    else:
        regs = int(m.group(1).replace(",", ""))
        if regs > a.max_registers:
            bad.append(
                f"{regs} registers (rail {a.max_registers}): a memory "
                f"has fallen out of BRAM inference -- look for an array "
                f"with two write ports or a read the RAM cannot serve")

    m = re.search(r"M10K blocks\s*;\s*([\d,]+)", text)
    if m:
        m10k = int(m.group(1).replace(",", ""))
        if m10k > a.max_m10k:
            bad.append(f"{m10k} M10K blocks over the device's {a.max_m10k}")

    if bad:
        print("map_gate: the synthesis result cannot fit; "
              "not paying for a fit to prove it")
        for line in bad:
            print(f"  {line}")
        return 1

    print(f"map ok: registers and memory inside the rails")
    return 0


if __name__ == "__main__":
    sys.exit(main())
