#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

from rp6502_asm import (RW0_ADDR, RW0_DATA, RW0_STEP, Asm, putc,
                        putnib, puthex)
from rp6502_rom import main, rom, script

GAMEPAD = 0xFF00
PLAYERS = 4
RECORD = 10
DPAD = 0
CONNECTED = 0x80

BUF = 0x0200

# The highest place for the block that leaves room for all four records.
LAST = 0x10000 - PLAYERS * RECORD
RESULT = 0x0200
DONE = 0x0204


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


def bounds_prog():
    p = Asm()

    # Ask for the block one byte too high, then at the highest place, and keep
    # what each xreg returns in A and X.
    p.xreg(0, 0, 2, LAST + 1)
    p.sta_abs(RESULT)
    p.stx_abs(RESULT + 1)
    p.xreg(0, 0, 2, LAST)
    p.sta_abs(RESULT + 2)
    p.stx_abs(RESULT + 3)
    p.store(DONE, 1)
    p.symbol("spin")
    p.bra("spin")
    return p


GAMEPAD_SCRIPT = r"""
run 40
expect "Disconnected"
expect-not "Select"
# The test runs with --memfill $55, so these read zero only because the
# program's xreg cleared all four records.
peek xram:GAMEPAD $00 $00 $00 $00 $00 $00 $00 $00 $00 $00
peek xram:GAMEPAD+3*RECORD $00 $00 $00 $00 $00 $00 $00 $00 $00 $00

pad 0 connect
pad 0 press start
run 20
expect "Select"
peek xram:GAMEPAD $80 $00 $00 $08

pad 0 release start
pad 0 press up a
run 10
peek xram:GAMEPAD $81 $00 $01 $00

# Each player's record is ten bytes, so player 1's starts at GAMEPAD+RECORD.
peek xram:GAMEPAD+RECORD $00

# The sticks byte holds the left stick's direction in its low nibble and the
# right stick's in its high nibble, and up is bit 0 of each nibble, so the byte
# reads $01 when the left stick is pushed fully up and the right stick is
# centered.
pad 0 stick 0 -127 0 0
run 10
peek xram:GAMEPAD $81 $01

# Left is 4 in the dpad nibble, and letting it go keeps the connected bit.
pad 0 release up
pad 0 press left
peek xram:GAMEPAD $84
pad 0 release left
peek xram:GAMEPAD $80

pad 0 disconnect
run 20
expect "Disconnected"
peek xram:GAMEPAD $00 $00 $00 $00 $00 $00 $00 $00 $00 $00

pad 1 connect playstation sticks
run 10
peek xram:GAMEPAD+RECORD $F0
pad 1 connect eastern
run 10
peek xram:GAMEPAD+RECORD $A0
pad 1 connect
run 10
peek xram:GAMEPAD+RECORD $80

# A stop leaves XRAM alone and drops the block, so a pad writes nothing there.
poke xram:GAMEPAD $AB
sys stop
pad 0 connect
peek xram:GAMEPAD $AB
"""

GAMEPAD_BOUNDS_SCRIPT = r"""
# gamepad_bounds.rp6502 keeps what its two xreg calls return at RESULT and
# sets DONE when it is done.

wait DONE $01
# Four ten-byte records must end by $FFFF, so $FFD9 is refused with -1 and
# $FFD8 is taken with 0.
peek RESULT $FF $FF $00 $00
# Player 3's record is the last ten bytes.
pad 3 connect
peek xram:LAST+3*RECORD $80
"""

GAMEPAD_HELD_SCRIPT = r"""
# A pad connected and held before the program maps the gamepad block is still
# there after it does.

pad 0 connect
pad 0 press start
run 40
expect "Select"
peek xram:GAMEPAD $80 $00 $00 $08
"""


def test_gamepad():
    # Only xreg writes some of the bytes that the script checks, so the fill
    # gives those bytes a known value.
    script(GAMEPAD_SCRIPT, "--memfill", "$55", rom("gamepad"))


def test_gamepad_bounds():
    # The fill sets DONE to $55, so the wait passes only after the program
    # sets DONE.
    script(GAMEPAD_BOUNDS_SCRIPT, "--memfill", "$55", rom("gamepad_bounds"))


def test_gamepad_held():
    script(GAMEPAD_HELD_SCRIPT, rom("gamepad"))


ROMS = {"gamepad": prog, "gamepad_bounds": bounds_prog}
TESTS = {"gamepad": test_gamepad, "gamepad_bounds": test_gamepad_bounds,
         "gamepad_held": test_gamepad_held}
main(globals())
