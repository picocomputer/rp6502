#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The results stay in RAM, where the script reads them while it types each
# Ctrl-C.

from rp6502_asm import OP_ATTR_GET, Asm
from rp6502_rom import main, rom, script

RIA_VSYNC = 0xFFE3
RIA_IRQ = 0xFFF0
IRQ_SIGINT = 0x40
ATTR_SIGINT = 0x08

STAGE = 0x0200
LATCH = 0x0201
EARLY = 0x0203
FLAGS = 0x0204
TAKEN = 0x0205
GO = 0x0210


def prog():
    p = Asm()
    p.jmp_abs("main")

    irq = p.here()
    p.pha()
    p.lda_abs(RIA_IRQ)
    p.and_imm(IRQ_SIGINT)
    p.sta_abs(FLAGS)
    # The count stops at $FF, so an interrupt that keeps coming back cannot
    # wrap around to 1.
    p.inc_abs(TAKEN)
    with p.branch("bne"):
        p.dec_abs(TAKEN)
    p.pla()
    p.rti()

    p.symbol("main")
    p.ldx_imm(TAKEN - STAGE)
    p.symbol("clear")
    p.stz_abx(STAGE)
    p.dex()
    p.bpl("clear")
    p.stz_abs(GO)

    # Read the latch until a Ctrl-C sets it, then once more.
    p.symbol("poll")
    p.call_a(OP_ATTR_GET, ATTR_SIGINT)
    p.cmp_imm(0)
    p.beq("poll")
    p.sta_abs(LATCH)
    p.call_a(OP_ATTR_GET, ATTR_SIGINT)
    p.sta_abs(LATCH + 1)
    p.store(STAGE, 1)

    # Interrupts are on while a Ctrl-C comes with SIGINT off, and the write
    # that turns SIGINT on clears that Ctrl-C, so a count here is a mistake.
    p.cli()
    p.symbol("go")
    p.lda_abs(GO)
    p.beq("go")
    p.store(RIA_IRQ, IRQ_SIGINT)
    p.lda_abs(RIA_VSYNC)
    p.clc()
    p.adc_imm(2)
    p.symbol("idle")
    p.cmp_abs(RIA_VSYNC)
    p.bne("idle")
    p.lda_abs(TAKEN)
    p.sta_abs(EARLY)
    p.store(STAGE, 2)
    p.symbol("spin")
    p.bra("spin")

    p.rom.add_irq_vector(irq)
    return p


SIGINT_SCRIPT = r"""
# sigint.rp6502 keeps what it sees of each Ctrl-C in RAM. STAGE is how far
# it has got, and it waits for a poke to GO before turning the interrupt on.

run 2
key c+ctrl
wait STAGE 1 2
# ATTR_SIGINT read 1 for the Ctrl-C, then 0 because reading it cleared it.
peek LATCH $01 $00

# This Ctrl-C comes while interrupts are on and SIGINT is off, so it does
# not interrupt.
key c+ctrl
run 3
peek TAKEN 0
# It is still waiting when the program turns SIGINT on, and the write that
# turns it on clears it, so no interrupt is taken for it.
poke GO 1
wait STAGE 2 10
peek EARLY 0

# Now a Ctrl-C interrupts. The handler's read of RIA_IRQ clears it, so the
# count of interrupts at TAKEN stays at 1.
key c+ctrl
wait TAKEN 1 2
run 2
peek FLAGS IRQ_SIGINT $01
"""


def test_sigint():
    script(SIGINT_SCRIPT, rom("sigint"))


ROMS = {"sigint": prog}
TESTS = {"sigint": test_sigint}
main(globals())
