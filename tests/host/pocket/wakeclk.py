#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

from rp6502_asm import OP_ATTR_GET, Asm
from rp6502_rom import main

# The program's clock in milliseconds.
ATTR_CLK_RUN_MS = 0x10
# The low half of the last reading, low byte first.
CLK = 0x10


def build():
    """Reads the program's clock over and over and keeps each reading."""
    p = Asm()
    p.symbol("read")
    p.call_a(OP_ATTR_GET, ATTR_CLK_RUN_MS)
    p.sta_zp(CLK)
    p.stx_zp(CLK + 1)
    p.jmp_abs("read")
    return p


ROMS = {"wakeclk": build}
main(globals())
