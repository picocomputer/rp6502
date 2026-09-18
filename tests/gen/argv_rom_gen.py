#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rp6502_script  # noqa: E402
from rp6502_asm import OP_ARGV, XSTACK, Asm, putc, putnib, puthex
from rp6502_rom import image

BAR = ord("|")


def prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex)
    p.symbol("main")

    p.say("argv[")
    p.call(OP_ARGV)

    p.tax()
    p.beq("done")
    p.symbol("emit")
    p.lda_abs(XSTACK)
    p.bne("show")
    p.lda_imm(BAR)
    p.symbol("show")
    p.putc_a()
    p.dex()
    p.bne("emit")

    p.symbol("done")
    p.say("]\r\n")
    p.stp()
    return p


def drive(emu, rom):
    def body(e):
        e.cmd('wait ".rp6502|"')
    return rp6502_script.drive(emu, rom, body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    ap.add_argument("--drive", action="store_true",
                    help="run the ROM on the emulator and check what it says")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the .rp6502 --emit wrote")
    a = ap.parse_args()
    if a.emit:
        print(f"argv.rp6502 {image(prog()).write(a.emit)} bytes")
    if a.drive:
        return drive(a.emu, a.rom)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
