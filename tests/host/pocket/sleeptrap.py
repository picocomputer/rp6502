#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Programs for the sleep and wake trips of test_psleep.cpp.

from rp6502_asm import API_A, OP_CLOSE, Asm, putc
from rp6502_rom import main

# A descriptor no program has open.
NO_FD = 15
COUNT = 0x10
CALLS = 1024


def trap():
    """Closes a descriptor that is not open, over and over. Every call
    fails, and the firmware writes the errno as a halfword at an odd
    address, which its trap handler performs a byte at a time. So the
    firmware spends this program in and out of that handler."""
    p = Asm()
    p.jmp_abs("main")
    p.use(putc)
    p.symbol("main")
    p.say("W")
    p.store(COUNT, 0)
    p.store(COUNT + 1, 0)
    p.symbol("loop")
    p.call_a(OP_CLOSE, NO_FD)
    p.cpx_imm(0xFF)
    p.beq("failed")
    p.say("\r\nCLOSE DID NOT FAIL\r\n")
    p.stp()
    p.symbol("failed")
    p.inc16(COUNT, COUNT + 1)
    p.lda_zp(COUNT + 1)
    p.cmp_imm(CALLS >> 8)
    p.beq("done")
    p.jmp_abs("loop")
    p.symbol("done")
    p.say("trap ok\r\n")
    p.stp()
    return p


ROMS = {"sleeptrap": trap}
main(globals())
