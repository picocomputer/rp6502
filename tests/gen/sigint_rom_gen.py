#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The results stay in RAM, where sigint.txt reads them while it types each
# Ctrl-C.

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import OP_ATTR_GET, Asm  # noqa: E402
from rp6502_rom import image  # noqa: E402

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

    rom = image(p)
    rom.add_irq_vector(irq)
    return rom


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", required=True)
    a = ap.parse_args()
    print(f"sigint.rp6502 {prog().write(a.emit)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
