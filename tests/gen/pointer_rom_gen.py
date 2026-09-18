#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import (RW0_ADDR, RW0_DATA, RW0_STEP, RW1_ADDR, RW1_DATA,  # noqa: E402
                        RW1_STEP, VIA_ACR, VIA_IER, VIA_IFR, VIA_T1_HI,
                        VIA_T1_LO, VIA_T1L_HI, VIA_T1L_LO, Asm)
from rp6502_rom import image  # noqa: E402

MOUSE = 0xFF00          # buttons, x, y, wheel, pan
TABLET = 0xFF10         # 4 byte header + 8 contacts of 6
MIRROR = 0xFF50
TICKS = 0xFF55

MOUSE_BYTES = 5

# A latch value of 65535 gives a period of about 8.2 ms at the default 8 MHz.
T1_PERIOD = 0xFFFF
ACR_T1_FREE_RUN = 0x40
IER_SET_T1 = 0xC0
IFR_T1 = 0x40


def tablet_rom():
    p = Asm()
    p.xreg(0, 0, 3, TABLET)
    p.jmp_abs(p.here())
    return image(p)


def mouse_rom():
    p = Asm()
    p.jmp_abs("main")

    irq = p.here()
    p.pha()
    p.phx()
    p.store(VIA_IFR, IFR_T1)  # a 1 written to an IFR bit clears that flag

    p.store(RW0_STEP, 1)
    p.store(RW0_ADDR, MOUSE & 0xFF)
    p.store(RW0_ADDR + 1, MOUSE >> 8)
    p.store(RW1_STEP, 1)
    p.store(RW1_ADDR, MIRROR & 0xFF)
    p.store(RW1_ADDR + 1, MIRROR >> 8)
    p.ldx_imm(0)
    p.symbol("copy")
    p.lda_abs(RW0_DATA)
    p.sta_abs(RW1_DATA)
    p.inx()
    p.cpx_imm(MOUSE_BYTES)
    p.bne("copy")

    p.store(RW1_STEP, 0)
    p.store(RW1_ADDR, TICKS & 0xFF)
    p.store(RW1_ADDR + 1, TICKS >> 8)
    p.lda_abs(RW1_DATA)
    p.inc_a()
    p.sta_abs(RW1_DATA)

    p.plx()
    p.pla()
    p.rti()

    p.symbol("main")
    p.xreg(0, 0, 1, MOUSE)  # reg 1 is the mouse, reg 3 the tablet

    p.store(VIA_T1L_LO, T1_PERIOD & 0xFF)
    p.store(VIA_T1L_HI, T1_PERIOD >> 8)
    p.store(VIA_T1_LO, T1_PERIOD & 0xFF)
    p.store(VIA_T1_HI, T1_PERIOD >> 8)
    p.store(VIA_ACR, ACR_T1_FREE_RUN)
    p.store(VIA_IER, IER_SET_T1)
    p.cli()

    p.jmp_abs(p.here())

    out = image(p)
    out.add_irq_vector(irq)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit-mouse")
    ap.add_argument("--emit-tablet")
    a = ap.parse_args()
    if a.emit_mouse:
        print(f"mouse.rp6502 {mouse_rom().write(a.emit_mouse)} bytes")
    if a.emit_tablet:
        print(f"tablet.rp6502 {tablet_rom().write(a.emit_tablet)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
