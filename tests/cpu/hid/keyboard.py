#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The exit code is 2 because the machine sets code 1 itself when an exec fails
# to boot.

from rp6502_asm import (API_A, API_X, OP_EXIT, RW0_ADDR, RW0_DATA, RW0_STEP,
                        Asm)
from rp6502_rom import main, rom, script

KEYBOARD = 0xFF10
EXIT_CODE = 2


def prog():
    p = Asm()
    p.xreg(0, 0, 0, KEYBOARD)
    p.store(RW0_STEP, 0)
    p.store(RW0_ADDR, KEYBOARD & 0xFF)
    p.store(RW0_ADDR + 1, KEYBOARD >> 8)

    # Bit 0 of the bitmap is set while no key is down.
    p.symbol("idle")
    p.lda_abs(RW0_DATA)
    p.lsr_a()
    p.bcs("idle")
    p.symbol("held")
    p.lda_abs(RW0_DATA)
    p.lsr_a()
    p.bcc("held")

    p.store(API_A, EXIT_CODE)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()
    return p


KEYBOARD_SCRIPT = r"""
# keyboard.rp6502 polls the keyboard bitmap in XRAM and exits with EXIT_CODE
# once a key has been pressed and released.

run 20
press space
run 5
release space
wait exit
expect exit EXIT_CODE
"""


def test_keyboard():
    script(KEYBOARD_SCRIPT, rom("keyboard"))


ROMS = {"keyboard": prog}
TESTS = {"keyboard": test_keyboard}
main(globals())
