#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The reference image for test_asm.cpp: one program that uses every
# instruction src/gen/rp6502_rom.py and tests/bench/tb_asm.h both carry,
# written in the Python spelling.
#
# There are two assemblers because there are two places a .rp6502 gets
# built — a generator writing a file, a bench building one in memory —
# and they claim to be one instruction set. This is the only thing that
# says so. It is not a program that runs; it is a program that encodes,
# and the encoding is the whole claim.

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "src" / "gen"))
from rp6502_rom import (API_A, ORG, OP_CLOSE, OP_OPEN, O_CREAT, O_WRONLY,
                        RIA_TX, Asm, Rom)  # noqa: E402


def build():
    p = Asm()

    # Immediates and absolutes, every register.
    p.lda(0x12)
    p.ldx(0x34)
    p.ldy(0x56)
    p.lda_abs(0x0200)
    p.ldx_abs(0x0201)
    p.sta(0x0202)
    p.stx(0x0203)
    p.inc_abs(0x0204)

    # Control flow, and the address arithmetic behind here().
    top = p.here()
    p.inx()
    p.dex()
    p.bit(0x0205)
    p.jsr(top)
    p.jmp(top)
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
    p.lda(O_WRONLY | O_CREAT)
    p.sta(API_A)
    p.call(OP_OPEN)
    p.call_a(OP_CLOSE, 0x03)
    p.xreg(1, 0, 0, 0x0001)
    p.xreg(1, 0, 1, 0x0003, 0x0000, 0x1000)

    # The two device idioms.
    p.poke(0x8000, 0x28)
    p.lda(0x07)
    p.sta(RIA_TX)
    p.putc_a()

    # Raw bytes, and the payload after the code.
    p.emit(0xEA, 0xEA)
    p.stp()
    p.b += b"asm\0"
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", required=True)
    a = ap.parse_args()
    # An XRAM record too, so the container's second address width is
    # compared and not only the program's.
    r = Rom().program(build(), ORG).record(0x10040, bytes(range(16)))
    print(f"asm_ref.rp6502 {r.write(a.emit)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
