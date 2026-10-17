#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

from pathlib import Path

import rp6502
from rp6502_asm import (API_A, OP_CLOSE, OP_OPEN, OP_READ_XSTACK, O_RDONLY,
                        XSTACK, Asm)
from rp6502_rom import main, rom

NAME = "M.DAT"
CHUNK = 16
DONE = b"stream ok\r\n"

HANDLE = 0x0200


def prog():
    p = Asm()

    p.push_str(NAME)
    p.lda_imm(O_RDONLY)
    p.sta_abs(API_A)
    p.call(OP_OPEN)
    p.sta_abs(HANDLE)

    p.symbol("loop")
    p.push(0)
    p.push(CHUNK)
    p.lda_abs(HANDLE)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)

    p.tax()
    with p.branch("beq"):
        p.symbol("inner")
        p.lda_abs(XSTACK)
        p.putc_a()
        p.dex()
        p.bne("inner")
        p.jmp_abs("loop")

    p.lda_abs(HANDLE)
    p.sta_abs(API_A)
    p.call(OP_CLOSE)
    for c in DONE:
        p.lda_imm(c)
        p.putc_a()
    p.stp()
    return p


def test_stream():
    # The payload has no zero byte because `wait` searches the program's
    # stdout as a C string, and a zero byte would hide everything printed
    # after it.
    payload = bytes(1 + (i * 7 + 11) % 0xFF for i in range(CHUNK * 5))
    Path(NAME).write_bytes(payload)
    with rp6502.Emu(rom("stream")) as e:
        e.cmd(f'wait "{DONE.decode().rstrip()}"')


ROMS = {"stream": prog}
TESTS = {"stream": test_stream}
main(globals())
