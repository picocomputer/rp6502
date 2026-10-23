#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

from rp6502_asm import (API_A, OP_CLOSE, OP_OPEN, OP_READ_XSTACK, O_RDONLY,
                        XSTACK, Asm, putc)
from rp6502_rom import main, rom, script

FD = 0x0200

END = "."


def prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc)

    p.symbol("main")
    p.push_str("TTY:")
    p.lda_imm(O_RDONLY)
    p.sta_abs(API_A)
    p.call(OP_OPEN)
    p.sta_abs(FD)

    p.symbol("poll")
    p.push(0)
    p.push(8)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)
    p.tax()
    with p.branch("beq"):
        p.symbol("echo")
        p.lda_abs(XSTACK)
        p.sta_abs(0x0201)
        p.jsr_abs("putc")
        p.lda_abs(0x0201)
        p.cmp_imm(ord(END))
        with p.branch("bne"):
            p.lda_abs(FD)
            p.sta_abs(API_A)
            p.call(OP_CLOSE)
            p.stp()
        p.dex()
        p.bne("echo")
    p.jmp_abs("poll")
    return p


TTY_KEYS_SCRIPT = r"""
run 10
key a+ctrl
wait "\x01"
type "\2"
wait "\x02"
type "\x41\102"
wait "AB"
type "\r\n"
wait "\r\n"
key space
wait " "
key minus
wait "-"
key 3+shift
wait "#"
key kp5
wait "5"
key a+shift
wait "A"
type "console reads."
wait "console reads."
"""


def test_tty_keys():
    script(TTY_KEYS_SCRIPT, rom("tty_keys"))


ROMS = {"tty_keys": prog}
TESTS = {"tty_keys": test_tty_keys}
main(globals())
