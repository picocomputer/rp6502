#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import (API_A, API_X, OP_ARGV, OP_EXEC, OP_EXIT, RW0_DATA,
                        XSTACK, Asm)  # noqa: E402
from rp6502_rom import image  # noqa: E402

ARG = "Foo"
BUF = 0x1000
SIZE = 0x00
END = 0x02
PTR = 0x04


def exit_with(p, code):
    p.store(API_A, code)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()


def prog():
    p = Asm()

    p.call(OP_ARGV)
    p.sta_zp(SIZE)
    p.stx_zp(SIZE + 1)
    p.clc()
    p.adc_imm(BUF & 0xFF)
    p.sta_zp(END)
    p.lda_zp(SIZE + 1)
    p.adc_imm(BUF >> 8)
    p.sta_zp(END + 1)

    p.lda_imm(BUF & 0xFF)
    p.sta_zp(PTR)
    p.lda_imm(BUF >> 8)
    p.sta_zp(PTR + 1)
    p.symbol("copy")
    p.lda_zp(PTR)
    p.cmp_zp(END)
    with p.branch("bne"):
        p.lda_zp(PTR + 1)
        p.cmp_zp(END + 1)
        p.beq("copied")
    p.lda_abs(XSTACK)
    p.sta_izp(PTR)
    p.inc16(PTR, PTR + 1)
    p.bra("copy")
    p.symbol("copied")

    p.lda_abs(BUF + 2)
    p.ora_abs(BUF + 3)
    with p.branch("bne"):
        p.jmp_abs("argc1")
    p.lda_abs(BUF + 4)
    p.ora_abs(BUF + 5)
    with p.branch("bne"):
        p.jmp_abs("argc2")
    exit_with(p, 1)

    p.symbol("argc1")
    p.poke(0x0000, 1)
    p.push_str(ARG)
    p.lda_zp(END)
    p.sta_zp(PTR)
    p.lda_zp(END + 1)
    p.sta_zp(PTR + 1)
    p.symbol("path")
    p.lda_zp(PTR)
    with p.branch("bne"):
        p.dec_zp(PTR + 1)
    p.dec_zp(PTR)
    p.lda_izp(PTR)
    p.sta_abs(XSTACK)
    p.lda_zp(PTR)
    p.cmp_imm((BUF + 4) & 0xFF)
    p.bne("path")
    p.lda_zp(PTR + 1)
    p.cmp_imm((BUF + 4) >> 8)
    p.bne("path")
    p.push(0)
    p.push(0)
    # The new offset table is one entry longer than the one read, so argv[0]
    # moves from 4 to 6 and argv[1] starts at SIZE + 2.
    p.lda_zp(SIZE)
    p.clc()
    p.adc_imm(2)
    p.tax()
    p.lda_zp(SIZE + 1)
    p.adc_imm(0)
    p.sta_abs(XSTACK)
    p.stx_abs(XSTACK)
    p.pushw(6)
    p.call(OP_EXEC)
    exit_with(p, 1)

    p.symbol("argc2")
    p.poke(0x0001, 2)
    p.lda_abs(BUF + 2)
    p.clc()
    p.adc_imm(BUF & 0xFF)
    p.sta_zp(PTR)
    p.lda_abs(BUF + 3)
    p.adc_imm(BUF >> 8)
    p.sta_zp(PTR + 1)
    p.ldy_imm(0)
    p.symbol("arg")
    p.lda_izy(PTR)
    p.sta_abs(RW0_DATA)
    with p.branch("beq"):
        p.iny()
        p.bne("arg")
    exit_with(p, 0)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    a = ap.parse_args()
    if a.emit:
        print(f"exec.rp6502 {image(prog()).write(a.emit)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
