#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The program reads 0 bytes from stdin, keeps what the read returned, and sets
# a byte to say it got past the read. Then it spins without reading again.

from rp6502_asm import OP_READ_XSTACK, Asm
from rp6502_rom import main, rom, script

DONE = 0x0200
RET = 0x0201


def prog():
    p = Asm()
    p.push(0)
    p.push(0)
    p.call_a(OP_READ_XSTACK, 0)
    p.sta_abs(RET)
    p.stx_abs(RET + 1)
    p.store(DONE, 1)
    p.symbol("spin")
    p.jmp_abs("spin")
    return p


ZEROREAD_SCRIPT = r"""
# The program reads 0 bytes from stdin, keeps what the read returned at
# RET, then sets DONE. A read that waited for a line would never get there.
# DONE holds a random byte until the program sets it, so the script runs one
# frame first.
run 1
wait DONE 1 10
peek RET 0 0

# Nothing is reading stdin now, so nothing echoes a typed key.
type "q"
run 2
expect-not console "q"
"""


def test_zeroread():
    script(ZEROREAD_SCRIPT, rom("zeroread"))


ROMS = {"zeroread": prog}
TESTS = {"zeroread": test_zeroread}
main(globals())
