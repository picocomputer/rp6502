#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The result is left in XRAM because the libretro test bench has no console
# that a suite can read.

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import (OP_GMTIME, OP_STRFTIME, RW0_ADDR, RW0_DATA, XSTACK,
                        Asm)  # noqa: E402
from rp6502_rom import image  # noqa: E402

# 1735732800 is 2025-01-01 12:00:00 UTC.
TIME = 1735732800
FORMAT = "%Y-%m-%d %H:%M:%S"
TM = 0x0200
TM_SIZE = 18
LEN = TM + TM_SIZE


def prog():
    p = Asm()

    p.pushl(TIME)
    p.call(OP_GMTIME)
    p.ldx_imm(0)
    p.symbol("pop")
    p.lda_abs(XSTACK)
    p.sta_abx(TM)
    p.inx()
    p.cpx_imm(TM_SIZE)
    p.bne("pop")

    # strftime reads the struct in the layout gmtime pushed it in, so the
    # bytes are pushed back last byte first.
    p.symbol("push")
    p.dex()
    p.lda_abx(TM)
    p.sta_abs(XSTACK)
    p.cpx_imm(0)
    p.bne("push")
    p.push_str(FORMAT)
    p.call(OP_STRFTIME)

    p.sta_abs(LEN)
    p.store(RW0_ADDR, 0)
    p.store(RW0_ADDR + 1, 0)
    p.lda_abs(LEN)
    p.sta_abs(RW0_DATA)
    p.ldx_imm(0)
    p.cpx_abs(LEN)
    p.beq("done")
    p.symbol("copy")
    p.lda_abs(XSTACK)
    p.sta_abs(RW0_DATA)
    p.inx()
    p.cpx_abs(LEN)
    p.bne("copy")
    p.symbol("done")
    p.stp()
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", required=True)
    a = ap.parse_args()
    print(f"time.rp6502 {image(prog()).write(a.emit)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
