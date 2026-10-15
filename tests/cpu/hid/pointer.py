#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

from rp6502_asm import (RW0_ADDR, RW0_DATA, RW0_STEP, RW1_ADDR, RW1_DATA,
                        RW1_STEP, VIA_ACR, VIA_IER, VIA_IFR, VIA_T1_HI,
                        VIA_T1_LO, VIA_T1L_HI, VIA_T1L_LO, Asm)
from rp6502_rom import main, rom, script

MOUSE = 0xFF00          # buttons, x, y, wheel, pan
TABLET = 0xFF10         # 4 byte header + 8 contacts of 4
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
    return p


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
    # XRAM holds random bytes at boot, so the program clears the count.
    p.store(RW1_ADDR, TICKS & 0xFF)
    p.store(RW1_ADDR + 1, TICKS >> 8)
    p.store(RW1_DATA, 0)

    p.store(VIA_T1L_LO, T1_PERIOD & 0xFF)
    p.store(VIA_T1L_HI, T1_PERIOD >> 8)
    p.store(VIA_T1_LO, T1_PERIOD & 0xFF)
    p.store(VIA_T1_HI, T1_PERIOD >> 8)
    p.store(VIA_ACR, ACR_T1_FREE_RUN)
    p.store(VIA_IER, IER_SET_T1)
    p.cli()

    p.jmp_abs(p.here())

    p.rom.add_irq_vector(irq)
    return p


POINTER_SCRIPT = r"""
# mouse.rp6502 maps the mouse block at MOUSE. Its 6522 timer interrupt copies
# the block's five bytes to MIRROR and adds one to a count at TICKS. Each wait
# is on the copy at MIRROR, so it passes only after an interrupt has read the
# new values.

run 10
peek xram:MOUSE $00 $00 $00 $00 $00

# Timer 1 free-runs from $FFFF, so it interrupts about every 8 ms at the
# default 8 MHz clock and ten 60 Hz frames hold twenty interrupts. The count
# starts from zero because the program clears it.
peek xram:TICKS $14

mouse buttons 1
wait xram:MIRROR $01
peek xram:MOUSE $01 $00 $00 $00 $00

mouse move 60 40
wait xram:MIRROR+1 $3C
peek xram:MOUSE $01 $3C $28
peek xram:MIRROR $01 $3C $28

mouse move 60 40
wait xram:MIRROR+1 $78
peek xram:MIRROR $01 $78 $50

mouse wheel 3
wait xram:MIRROR+3 $03
peek xram:MOUSE $01 $78 $50 $03 $00

mouse buttons 0
wait xram:MIRROR $00
peek xram:MIRROR $00 $78 $50 $03 $00
"""

TABLET_SCRIPT = r"""
# tablet.rp6502 maps the tablet block at TABLET. Each contact is flags, y_lo,
# x_lo, yx_hi.
let contact = TABLET + 4
run 10
peek xram:contact $00 $00 $00 $00

# A single touch is contact 0 with its flags set to $01, TABLET_FLAG_LEFT.
tablet touch 60,60
wait xram:contact $01
peek xram:contact $01 $3C $3C $00

# X 300 is $12C and Y 200 is $0C8.
tablet touch 300,200
wait xram:contact+2 $2C
peek xram:contact $01 $C8 $2C $01

# X 100 is $064 and Y 300 is $12C.
tablet touch 100,300
wait xram:contact+3 $10
peek xram:contact $01 $2C $64 $10

# A hovering pointer is also contact 0, and its flags are its button bitmap
# with TABLET_FLAG_HOVER, $80, set.
tablet at 100 100 1
wait xram:contact $81
peek xram:contact $81 $64 $64 $00

# A released contact has flags of 0 and keeps its last position.
tablet clear
wait xram:contact $00
peek xram:contact $00 $64 $64 $00

tablet touch 10,10 20,20 30,30
wait xram:contact $01
peek xram:contact $01 $0A $0A $00
peek xram:contact+4 $01 $14 $14 $00
peek xram:contact+8 $01 $1E $1E $00
"""


def test_pointer():
    script(POINTER_SCRIPT, rom("mouse"))


def test_tablet():
    script(TABLET_SCRIPT, rom("tablet"))


ROMS = {"mouse": mouse_rom, "tablet": tablet_rom}
TESTS = {"pointer": test_pointer, "tablet": test_tablet}
main(globals())
