#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The results are copied to XRAM because the libretro test bench has no
# console that a suite can read.

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import RW0_ADDR, RW0_DATA, Asm  # noqa: E402
from rp6502_rom import image  # noqa: E402

RIA_VSYNC = 0xFFE3
RIA_IRQ = 0xFFF0
IRQ_VSYNC = 0x80

RES = 0x0200
POLLED = RES
TAKEN = RES + 3
IN_IRQ = RES + 4
DISABLED = RES + 5
STAMPS = RES + 6
RESULTS = 9


def prog():
    p = Asm()
    p.jmp_abs("main")

    irq = p.here()
    p.pha()
    p.phx()
    p.lda_abs(RIA_IRQ)
    p.sta_abs(IN_IRQ)
    p.ldx_abs(TAKEN)
    p.lda_abs(RIA_VSYNC)
    p.sta_abx(STAMPS)
    p.inx()
    p.stx_abs(TAKEN)
    p.plx()
    p.pla()
    p.rti()

    p.symbol("main")
    p.ldx_imm(0)
    p.lda_abs(RIA_VSYNC)
    p.symbol("poll")
    p.cmp_abs(RIA_VSYNC)
    p.beq("poll")
    p.lda_abs(RIA_VSYNC)
    p.sta_abx(POLLED)
    p.inx()
    p.cpx_imm(3)
    p.bne("poll")

    p.stz_abs(TAKEN)
    p.store(RIA_IRQ, IRQ_VSYNC)
    p.cli()
    p.symbol("taking")
    p.lda_abs(TAKEN)
    p.cmp_imm(3)
    p.bne("taking")

    p.stz_abs(RIA_IRQ)
    p.lda_abs(RIA_VSYNC)
    p.clc()
    p.adc_imm(2)
    p.symbol("idle")
    p.cmp_abs(RIA_VSYNC)
    p.bne("idle")
    p.lda_abs(RIA_IRQ)
    p.sta_abs(DISABLED)

    p.store(RW0_ADDR, 0)
    p.store(RW0_ADDR + 1, 0)
    p.ldx_imm(0)
    p.symbol("dump")
    p.lda_abx(RES)
    p.sta_abs(RW0_DATA)
    p.inx()
    p.cpx_imm(RESULTS)
    p.bne("dump")
    p.stp()

    rom = image(p)
    rom.add_irq_vector(irq)
    return rom


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", required=True)
    a = ap.parse_args()
    print(f"vsync.rp6502 {prog().write(a.emit)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
