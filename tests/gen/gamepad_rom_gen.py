#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import (RW0_ADDR, RW0_DATA, RW0_STEP, Asm, putc,  # noqa: E402
                        putnib, puthex)
from rp6502_rom import image  # noqa: E402

GAMEPAD = 0xFF00
PLAYERS = 4
RECORD = 10
DPAD = 0
CONNECTED = 0x80

BUF = 0x0200


def prog():
    p = Asm()
    p.jmp_abs("main")

    p.use(putc, putnib, puthex)

    p.symbol("main")
    p.xreg(0, 0, 2, GAMEPAD)
    p.say("\x18\x1bc\nPicocomputer 6502 Gamepad Tester\n")

    p.symbol("pass")
    p.say("\x1b[H\x1b[3B")
    p.store(RW0_STEP, 1)
    p.store(RW0_ADDR, GAMEPAD & 0xFF)
    p.store(RW0_ADDR + 1, GAMEPAD >> 8)
    p.stz_abs(BUF + RECORD)

    p.symbol("player")
    p.ldx_imm(0)
    p.symbol("fetch")
    p.lda_abs(RW0_DATA)
    p.sta_abx(BUF)
    p.inx()
    p.cpx_imm(RECORD)
    p.bne("fetch")

    p.say("P")
    p.lda_abs(BUF + RECORD)
    p.clc()
    p.adc_imm(ord("0"))
    p.jsr_abs("putc")
    p.say(" ")

    p.lda_abs(BUF + DPAD)
    p.and_imm(CONNECTED)
    with p.branch("beq"):
        p.say("Select ")
        p.ldx_imm(0)
        p.symbol("show")
        p.lda_abx(BUF)
        p.jsr_abs("puthex")
        p.inx()
        p.cpx_imm(4)
        p.bne("show")
        p.jmp_abs("drawn")
    p.say("Disconnected")

    p.symbol("drawn")
    p.say("\x1b[K\n")
    p.inc_abs(BUF + RECORD)
    p.lda_abs(BUF + RECORD)
    p.cmp_imm(PLAYERS)
    with p.branch("beq"):
        p.jmp_abs("player")
    p.jmp_abs("pass")
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    a = ap.parse_args()
    if a.emit:
        print(f"gamepad.rp6502 {image(prog()).write(a.emit)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
