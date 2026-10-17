#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import rp6502
from rp6502_asm import (API_A, OP_CLOSE, OP_OPEN, OP_READ_XSTACK,
                        OP_WRITE_XSTACK, O_CREAT, O_RDONLY, O_TRUNC,
                        O_WRONLY, XSTACK, Asm)
from rp6502_rom import main, rom

NAME = "SAVE:T.DAT"
PAYLOAD = b"pocket file ok\r\n"



def prog():
    p = Asm()

    p.push_str(NAME)
    p.lda_imm(O_WRONLY | O_CREAT | O_TRUNC)
    p.sta_abs(API_A)
    p.call(OP_OPEN)
    p.sta_abs(0x0200)

    for c in reversed(PAYLOAD):
        p.push(c)
    p.lda_abs(0x0200)
    p.sta_abs(API_A)
    p.call(OP_WRITE_XSTACK)

    p.lda_abs(0x0200)
    p.sta_abs(API_A)
    p.call(OP_CLOSE)

    p.push_str(NAME)
    p.lda_imm(O_RDONLY)
    p.sta_abs(API_A)
    p.call(OP_OPEN)
    p.sta_abs(0x0200)

    p.push(0)
    p.push(len(PAYLOAD))
    p.lda_abs(0x0200)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)

    p.tax()
    with p.branch("beq"):
        p.symbol("loop")
        p.lda_abs(XSTACK)
        p.putc_a()
        p.dex()
        p.bne("loop")

    p.lda_abs(0x0200)
    p.sta_abs(API_A)
    p.call(OP_CLOSE)
    p.stp()
    return p


def test_file():
    with rp6502.Emu(rom("file")) as e:
        e.cmd(f'wait "{PAYLOAD.decode().rstrip()}"')


ROMS = {"file": prog}
TESTS = {"file": test_file}
main(globals())
