#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The soft CPU's firmware, for the bitstream. Simulation loads it
# through the testbench, straight into the verilated arrays, so nothing
# in the build ever had to put it in the fabric — and for a long time
# nothing did. The tightly-coupled memory is four byte-wide arrays, so
# the image splits four ways, one file per lane, in the format
# $readmemh wants.

import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: rv_tcm_gen.py <sw.bin> <out-prefix> <words>",
              file=sys.stderr)
        return 2
    data = Path(sys.argv[1]).read_bytes()
    prefix = sys.argv[2]
    words = int(sys.argv[3])
    if len(data) > words * 4:
        print(f"rv_tcm_gen: {len(data)} bytes will not fit {words} words",
              file=sys.stderr)
        return 1
    data = data + bytes(words * 4 - len(data))
    for lane in range(4):
        Path(f"{prefix}.{lane}").write_text(
            "".join(f"{data[i * 4 + lane]:02x}\n" for i in range(words)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
