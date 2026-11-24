#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

from rp6502_asm import (RIA_READY, RIA_RX, RW0_ADDR, RW0_DATA, RW0_STEP,
                        RW1_ADDR, RW1_DATA, RW1_STEP, VIA_ACR, VIA_IER,
                        VIA_IFR, VIA_T1_HI, VIA_T1_LO, VIA_T1L_HI,
                        VIA_T1L_LO, XSTACK, OP_XREG, Asm, putc)
from rp6502_rom import main, rom, script

MOUSE = 0xFF00          # buttons, x, y, wheel, pan
TABLET = 0xFF10         # 4 byte header + 8 contacts of 4
MIRROR = 0xFF50
TICKS = 0xFF55

MOUSE_BYTES = 5

# The console program maps nothing. It says WORDS on the console's first row,
# for the console's mouse to select, writes back each byte it reads, so a
# paste shows after them, and stays on the console until a frontend writes a
# byte other than 0 here, and then selects a canvas with no mode 0.
LEAVE = 0x0200
WORDS = "select these words"

# The paced console program first sets the console's speed to 1200 baud
# with DECSCS, so what it says reaches the screen over several frames.
DECSCS_1200 = "\33[;3*r"

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


def console_rom(first=""):
    p = Asm()
    p.jmp_abs("main")
    p.use(putc)
    p.symbol("main")
    p.store(LEAVE, 0)
    p.say(first + WORDS)
    p.symbol("wait")
    p.bit_abs(RIA_READY)
    p.bvc("idle")
    p.lda_abs(RIA_RX)
    p.jsr_abs("putc")
    p.symbol("idle")
    p.lda_abs(LEAVE)
    p.beq("wait")
    p.xreg(1, 0, 0, 1)  # the 320x240 canvas
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


# The mode 6 program draws its own pointers. Its sprite array ends two bytes
# short of the end of XRAM, so shape 4 is a descriptor that runs past it and
# draws nothing. A left click steps the shape through 1 to 4 and then 0, no
# pointer, and a right click swaps the 320x240 and 640x480 canvases.
MODE6_CONFIG = 0xFFE0
MODE6_PALETTE = 0xF000
MODE6_TARGET = 0xE000
MODE6_ARROW = 0xE100
MODE6_SHAPES = 4

# 4 bits a pixel: 0 is clear, then white, black, red, green, blue and yellow,
# each with the alpha bit, bit 5, set.
MODE6_COLORS = [0x0000, 0xFFFF, 0x0020, 0x003F, 0x07E0, 0xF820, 0x07FF]

TARGET = """
.....111111.....
...1133333311...
..133......331..
.13..........31.
.13..........31.
13............31
13.....44.....31
13....4224....31
13....4224....31
13.....44.....31
13............31
.13..........31.
.13..........31.
..133......331..
...1133333311...
.....111111.....
"""

ARROW = """
2...............
22..............
212.............
2112............
21112...........
211112..........
2111112.........
21111112........
211111112.......
2111112222......
2112112.........
212.2112........
22..2112........
2....2112.......
.....2112.......
......22........
"""


def mode6_image(art):
    """The rows of art, 4 bits a pixel, the leftmost in the high nibble."""
    out = bytearray()
    for row in art.split():
        px = [0 if c == "." else int(c) for c in row]
        out += bytes(px[i] << 4 | px[i + 1] for i in range(0, len(px), 2))
    return out


def mode6_sprite(x, y, image, wh, options):
    return (x.to_bytes(2, "little", signed=True) +
            y.to_bytes(2, "little", signed=True) +
            image.to_bytes(2, "little") +
            MODE6_PALETTE.to_bytes(2, "little") + bytes((wh, options)))


def mode6_rom():
    p = Asm()
    shape, prev, now, pressed, canvas = 0, 1, 2, 3, 4  # zero page

    x = p.rom.add_binary_data
    x(b"".join(c.to_bytes(2, "little") for c in MODE6_COLORS),
      0x10000 + MODE6_PALETTE)
    x(mode6_image(TARGET), 0x10000 + MODE6_TARGET)
    x(mode6_image(ARROW), 0x10000 + MODE6_ARROW)
    # 16x16, 4 bits a pixel: the target with its hot spot at its centre, the
    # arrow doubled to 32x32 with its tip at 0,0, and the arrow flipped,
    # its tip at the top right.
    x(mode6_sprite(8, 8, MODE6_TARGET, 0x33, 0x02) +
      mode6_sprite(0, 0, MODE6_ARROW, 0x33, 0xC2) +
      mode6_sprite(15, 0, MODE6_ARROW, 0x33, 0x12),
      0x10000 + MODE6_CONFIG)

    p.xreg(0, 0, 3, TABLET)
    p.xreg(1, 0, 0, 1)  # the 320x240 canvas
    p.xreg(1, 0, 1, 6, MODE6_CONFIG)
    p.store(RW0_STEP, 0)
    p.store(RW0_ADDR, (TABLET + 4) & 0xFF)  # contact 0's flags
    p.store(RW0_ADDR + 1, (TABLET + 4) >> 8)
    p.store(RW1_STEP, 0)
    p.store(RW1_ADDR, TABLET & 0xFF)  # control, the shape
    p.store(RW1_ADDR + 1, TABLET >> 8)
    p.store(shape, 1)
    p.store(RW1_DATA, 1)
    p.store(prev, 0)
    p.store(canvas, 1)

    p.symbol("loop")
    p.lda_abs(RW0_DATA)
    p.sta_zp(now)
    p.eor_zp(prev)
    p.and_zp(now)
    p.sta_zp(pressed)
    p.lda_zp(now)
    p.sta_zp(prev)

    p.lda_zp(pressed)
    p.and_imm(1)
    p.beq("right")
    p.inc_zp(shape)
    p.lda_zp(shape)
    p.cmp_imm(MODE6_SHAPES + 1)
    p.bne("set")
    p.lda_imm(0)
    p.sta_zp(shape)
    p.symbol("set")
    p.sta_abs(RW1_DATA)

    p.symbol("right")
    p.lda_zp(pressed)
    p.and_imm(2)
    p.beq("loop")
    p.lda_zp(canvas)
    p.eor_imm(1 ^ 3)  # 320x240 and 640x480
    p.sta_zp(canvas)
    p.push(1)
    p.push(0)
    p.push(0)
    p.push(0)
    p.lda_zp(canvas)
    p.sta_abs(XSTACK)
    p.call(OP_XREG)
    p.jmp_abs("loop")
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

# Bit 0 of status is the program's. Mapping sets it to share the tablet,
# and the tablet never writes it after.
peek xram:TABLET+1 $01
poke xram:TABLET+1 $00
tablet at 120 80 0
wait xram:contact $80
peek xram:TABLET+1 $00

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


ROMS = {"mouse": mouse_rom, "tablet": tablet_rom, "console": console_rom,
        "console_paced": lambda: console_rom(DECSCS_1200), "mode6": mode6_rom}
TESTS = {"pointer": test_pointer, "tablet": test_tablet}
main(globals())
