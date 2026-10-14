#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The program reads 0 bytes from stdin, keeps what the read returned, and sets
# a byte to say it got past the read. Then it spins without reading again.

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import OP_READ_XSTACK, Asm  # noqa: E402
from rp6502_rom import image  # noqa: E402

DONE = 0x0200
RET = 0x0201


def prog():
    p = Asm()
    p.push(0)
    p.push(0)
    p.call_a(OP_READ_XSTACK, 0)
    p.sta_abs(RET)
    p.stx_abs(RET + 1)
    p.store(DONE, 1)
    p.symbol("spin")
    p.jmp_abs("spin")
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", required=True)
    a = ap.parse_args()
    print(f"zeroread.rp6502 {image(prog()).write(a.emit)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
