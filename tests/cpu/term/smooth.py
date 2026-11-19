#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""Smooth scroll (DECSCLM) demos and tests.

Each ROM plays a stream of bytes to the console through the RIA's TX
register, waiting on its ready bit, over and over. A byte that would scroll
a region still moving waits in the console's output, so the ready bit is
all the pacing a stream needs: it is written as fast as the 6502 goes.
"""

import random

from rp6502_asm import Asm, putc
from rp6502_rom import main, rom, script

PTR = 0x00

ESC = "\33"
CSI = ESC + "["
RESET = "\30" + ESC + "c"
SMOOTH = CSI + "?4h"
HIDE = CSI + "?25l"
DEC = ESC + "(0"
ASCII = ESC + "(B"


def at(row, col=1):
    return f"{CSI}{row};{col}H"


def region(top, bot):
    return f"{CSI}{top};{bot}r"


def player(stream, canvas=None, loop=True):
    """A ROM that writes stream to the console, then again from the start,
    or stops after once. It is assembled twice: the first time finds where
    the stream begins, after the code."""
    data = stream.encode("latin-1")
    if 0 in data:
        raise ValueError("a stream ends at its first NUL")
    begin = 0
    for _ in range(2):
        p = Asm()
        if canvas is not None:
            p.xreg(1, 0, 0, canvas)
            p.xreg(1, 0, 1, 0, 0, 0, 0)
        p.jmp_abs("main")
        p.use(putc)
        p.symbol("main")
        p.store(PTR, begin & 0xFF)
        p.store(PTR + 1, begin >> 8)
        p.symbol("next")
        p.lda_izp(PTR)
        p.beq("main" if loop else "done")
        p.jsr_abs("putc")
        p.inc_zp(PTR)
        p.bne("next")
        p.inc_zp(PTR + 1)
        p.jmp_abs("next")
        p.symbol("done")
        p.jmp_abs("done")
        begin = p.here()
    p.data(data + b"\0")
    return p


def full_screen():
    """The whole screen scrolls up, one line after another."""
    rng = random.Random(6502)
    words = ("smooth", "scroll", "one", "scanline", "every", "frame",
             "VT102", "Picocomputer", "6502", "region", "line", "feed")
    s = RESET + HIDE + SMOOTH
    for n in range(400):
        text = " ".join(rng.choice(words) for _ in range(rng.randint(4, 10)))
        s += f"{n:4}  {text}\r\n"
    return s


def box_rows(top, bot, width):
    s = DEC + at(top - 1) + "l" + "q" * (width - 2) + "k"
    for row in range(top, bot + 1):
        s += at(row) + "x" + at(row, width) + "x"
    s += at(bot + 1) + "m" + "q" * (width - 2) + "j" + ASCII
    return s


CREDITS = """Smooth scroll on the Picocomputer
as a VT102 had it: one scanline a frame
and only inside the box, while the
title above and the status below
stay where they are.

The text after each line feed goes into
the line coming in, and the next line
feed waits for this one to land, so the
lines never stop moving.

A program needs no flow control for
this. Its write simply takes as long as
the scroll does, as a VT100 user always
saw: six lines a second.

Line drawing comes from DEC Special
Graphics, and the box sides are written
again on every line, because a VT102
has no left and right margins.
""".splitlines()


def box():
    """Credits roll inside a line-drawing box, under a double-height title."""
    width = 80
    top, bot = 6, 20
    s = RESET + HIDE
    s += at(1, 22) + ESC + "#3" + CSI + "1mSMOOTH SCROLL" + CSI + "0m"
    s += at(2, 22) + ESC + "#4" + CSI + "1mSMOOTH SCROLL" + CSI + "0m"
    s += box_rows(top, bot, width)
    s += at(23, 3) + CSI + "7m DECSCLM " + CSI + "0m  CSI ? 4 h, then a region with DECSTBM"
    s += region(top, bot) + SMOOTH
    lines = CREDITS + [""] * (bot - top + 1)
    for line in lines:
        s += at(bot) + "\n\r" + DEC + "x" + ASCII + " " + line.ljust(width - 4)
        s += " " + DEC + "x" + ASCII
    return s


def snow():
    """Snow falls in a region while a sleigh flies over it."""
    rng = random.Random(1225)
    sleigh = [r"    __  __       ",
              r" __/_|_|_|___.-o ",
              r"(______________) ",
              r"   '--'  '--'    "]
    s = RESET + HIDE + SMOOTH + region(5, 22)
    s += at(23) + CSI + "7m" + " Snow falls down the region by reverse index ".center(80) + CSI + "0m"
    col = 64
    for frame in range(240):
        for i, row in enumerate(sleigh):
            s += at(1 + i, 1) + CSI + "2K" + at(1 + i, col) + row[:81 - col]
        col = col - 1 if col > 1 else 64
        s += at(5) + ESC + "M"
        line = ""
        for _ in range(80):
            r = rng.random()
            line += "*" if r < 0.04 else "." if r < 0.07 else " "
        s += CSI + "1m" + line + CSI + "0m"
    return s


def split():
    """The top half scrolls up while the bottom half scrolls down."""
    s = RESET + HIDE + SMOOTH
    s += at(12) + CSI + "7m" + " up above, down below ".center(80) + CSI + "0m"
    for n in range(120):
        s += region(1, 11) + at(11) + "\n" + f"\r  up   {n:3}  " + "^" * (n % 60)
        s += region(13, 24) + at(13) + ESC + "M" + f"\r  down {n:3}  " + "v" * (n % 60)
    return s


def forty():
    """The 40-column terminal: a row moves a scanline every second frame."""
    s = RESET + HIDE + SMOOTH
    for n in range(300):
        s += f"{n:3} forty columns, eight lines a row\r\n"
    return s


def rom_full():
    return player(full_screen())


def rom_box():
    return player(box())


def rom_snow():
    return player(snow())


def rom_split():
    return player(split())


def rom_forty():
    return player(forty(), canvas=1)


# A row takes 16 frames, so about 50 lines are out by frame 400: the 30 that
# fill the screen at once, then one every 16 frames. With nothing holding the
# output, all 400 would be.
PACED = """
run 400
expect "  40  "
expect-not " 100  "
"""


def test_paced():
    script(PACED, rom("smooth_full"))


ROMS = {"smooth_full": rom_full, "smooth_box": rom_box,
        "smooth_snow": rom_snow, "smooth_split": rom_split,
        "smooth_forty": rom_forty}
TESTS = {"smooth_paced": test_paced}
main(globals())
