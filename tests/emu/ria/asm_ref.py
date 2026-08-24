#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The reference program for test_asm.cpp: one program that uses every
# instruction tests/gen/rp6502_asm.py and tests/bench/tb_asm.h both carry,
# written in the Python spelling.
#
# There are two assemblers because there are two places a program gets
# built — a generator writing a file, a bench building one in memory. The
# Python one carries the whole instruction set and a symbol table; the
# C++ one carries what a bench parameterizes at run time. Where they
# overlap they are one encoding, and this is the only thing that says so.
# It is not a program that runs; it is a program that encodes.

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "gen"))
from rp6502_asm import (API_A, ORG, OP_CLOSE, OP_OPEN, O_CREAT, O_WRONLY,
                        RIA_TX, Asm)  # noqa: E402
from rp6502_rom import Rom  # noqa: E402


def build():
    p = Asm()

    # Immediates and absolutes, every register.
    p.lda_imm(0x12)
    p.ldx_imm(0x34)
    p.ldy_imm(0x56)
    p.lda_abs(0x0200)
    p.ldx_abs(0x0201)
    p.sta_abs(0x0202)
    p.stx_abs(0x0203)
    p.inc_abs(0x0204)

    # Control flow, and the symbol table behind a backward reference.
    p.symbol("top")
    p.inx()
    p.dex()
    p.bit_abs(0x0205)
    p.jsr_abs("top")
    p.jmp_abs("top")
    p.rts()

    # The xstack, in all four widths. The string is the one that would
    # break silently if the two disagreed about which end goes down
    # first, so it carries a byte over 0x7F.
    p.store(0x0206, 0x78)
    p.push(0x9A)
    p.pushw(0xBCDE)
    p.pushl(0x01234567)
    p.push_str("MSC0:\xff")

    # The API, both call shapes.
    p.push_str("T.DAT")
    p.lda_imm(O_WRONLY | O_CREAT)
    p.sta_abs(API_A)
    p.call(OP_OPEN)
    p.call_a(OP_CLOSE, 0x03)
    p.xreg(1, 0, 0, 0x0001)
    p.xreg(1, 0, 1, 0x0003, 0x0000, 0x1000)

    # The two device idioms.
    p.poke(0x8000, 0x28)
    p.lda_imm(0x07)
    p.sta_abs(RIA_TX)
    p.putc_a()

    # Raw bytes, and the payload after the code.
    p.nop()
    p.nop()
    p.stp()
    p.data(b"asm\0")
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", help="the assembled program, as a .rp6502")
    ap.add_argument("--emit-bin", help="the assembled program, raw")
    a = ap.parse_args()
    if a.emit:
        # An XRAM record too, so the container's second address width is
        # written and not only the program's.
        r = Rom().program(build(), ORG).record(0x10040, bytes(range(16)))
        print(f"asm_ref.rp6502 {r.write(a.emit)} bytes")
    if a.emit_bin:
        code = build().code()
        Path(a.emit_bin).write_bytes(code)
        print(f"asm_ref.bin {len(code)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
