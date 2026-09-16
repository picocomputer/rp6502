#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Two programs that map a pointing device into XRAM and stay running, so a
# test can work the host's mouse or tablet and read what the RIA wrote.
#
# One device each, because which device a program asks for is itself a claim:
# a frontend withholds the mouse until a program maps it, and a tablet program
# must never provoke that.
#
# The tablet needs nothing from the 6502: the RIA writes its block. The mouse
# is read back through a 6522 timer interrupt, the way an application must,
# because it reports motion as counters that wrap in a byte. The mirror the
# handler leaves in XRAM is a 6502's word for what the RIA wrote, and the tick
# counter beside it says the interrupt is still arriving.

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import (RW0_ADDR, RW0_DATA, RW0_STEP, RW1_ADDR, RW1_DATA,  # noqa: E402
                        RW1_STEP, VIA_ACR, VIA_IER, VIA_IFR, VIA_T1_HI,
                        VIA_T1_LO, VIA_T1L_HI, VIA_T1L_LO, Asm)
from rp6502_rom import image  # noqa: E402

# The blocks, and the mirror the interrupt writes. A test reads all three.
MOUSE = 0xFF00          # buttons, x, y, wheel, pan
TABLET = 0xFF10         # 4 byte header + 8 contacts of 6
MIRROR = 0xFF50         # what the interrupt saw of MOUSE
TICKS = 0xFF55          # interrupts taken, low byte

MOUSE_BYTES = 5

# Timer 1 free running, interrupt on. The period is left at its widest so the
# rate does not depend on the clock a test happens to run at: 65535 cycles is
# about 8 ms at the default 8 MHz, which is the rate the RIA docs ask a mouse
# to be read at.
T1_PERIOD = 0xFFFF
ACR_T1_FREE_RUN = 0x40
IER_SET_T1 = 0xC0
IFR_T1 = 0x40


def tablet_rom():
    """The absolute pointer, which the RIA publishes without help."""
    p = Asm()
    p.xreg(0, 0, 3, TABLET)
    p.jmp_abs(p.here())
    return image(p)


def mouse_rom():
    """The relative pointer, read back under the timer interrupt."""
    p = Asm()
    p.jmp_abs("main")

    irq = p.here()
    p.pha()
    p.phx()
    p.store(VIA_IFR, IFR_T1)  # acknowledge timer 1

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

    # Nothing left to do but be interruptible.
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
