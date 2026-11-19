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
import re
from pathlib import Path

from rp6502_asm import OP_EXIT, OP_SYNCFS, Asm, putc
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


def player(stream, canvas=None, loop=True, sync=False):
    """A ROM that writes stream to the console, then again from the start,
    or once and exits, after a sync of stdout with sync. It is assembled
    twice: the first time finds where the stream begins, after the code."""
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
        if sync:
            p.call_a(OP_SYNCFS, 1)
        p.call_a(OP_EXIT, 0)
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


# ---- The VT102 demo -------------------------------------------------------

def sgr(*codes):
    return CSI + ";".join(str(c) for c in codes) + "m"


C_KEYWORDS = {"if", "else", "for", "while", "do", "return", "break",
              "continue", "switch", "case", "default", "goto", "sizeof",
              "static", "const", "volatile", "inline", "extern", "struct",
              "union", "enum", "typedef"}
C_TYPES = {"void", "bool", "char", "int", "unsigned", "long", "short",
           "uint8_t", "uint16_t", "uint32_t", "int32_t", "size_t",
           "term_t", "term_grid_t", "term_pos_t", "cells_t", "screen_buf_t",
           "cells_smooth_t", "ansi_state_t", "mode0_cell_t"}
C_TOKEN = re.compile(r"""
    (?P<comment>//.*|/\*.*?(\*/|$))
  | (?P<string>"(\\.|[^"\\])*"|'(\\.|[^'\\])*')
  | (?P<number>\b0[xX][0-9A-Fa-f]+[uUlL]*\b|\b\d+[uUlL]*\b)
  | (?P<word>[A-Za-z_]\w*)
  | (?P<space>\s+)
  | (?P<other>.)
""", re.VERBOSE)
INK = {"keyword": sgr(38, 5, 75), "type": sgr(38, 5, 79),
       "pre": sgr(38, 5, 176), "string": sgr(38, 5, 180),
       "comment": sgr(3, 38, 5, 108), "number": sgr(38, 5, 151),
       "call": sgr(38, 5, 222), "macro": sgr(38, 5, 209),
       "plain": sgr(38, 5, 252), "punct": sgr(38, 5, 248)}


def c_colors(line, typo=None):
    """One line of C with its syntax in color. typo, a word of the line,
    gets a red curly underline, as an editor marks an error."""
    if line.lstrip().startswith("#"):
        return INK["pre"] + line + sgr(0)
    out = ""
    for m in C_TOKEN.finditer(line):
        kind, text = m.lastgroup, m.group()
        if kind == "word":
            after = line[m.end():].lstrip()[:1]
            if text in C_KEYWORDS:
                kind = "keyword"
            elif text in C_TYPES:
                kind = "type"
            elif text.isupper() and len(text) > 1:
                kind = "macro"
            elif after == "(":
                kind = "call"
            else:
                kind = "plain"
        elif kind == "other":
            kind = "punct"
        elif kind == "space":
            out += text
            continue
        if typo and text == typo:
            out += sgr(0) + INK[kind] + sgr("4:3", 58, 5, 196) + text + sgr(24, 59)
            continue
        out += sgr(0) + INK[kind] + text
    return out + sgr(0)


def listing():
    """The project's own smooth scroll code, cut to fit a screen."""
    root = Path(__file__).resolve().parents[3] / "src" / "core"
    picks = [("term/term.c", "static bool HOST_IN_RAM_FUNC term_byte_scrolls", "^}"),
             ("term/term.c", "bool HOST_IN_RAM_FUNC term_holds", "^}"),
             ("term/cells.c", "void cells_smooth(term_t", "^}"),
             ("vga/mode/mode0.h", "static inline unsigned mode0_smooth_moved", "^}")]
    lines = []
    for name, first, last in picks:
        text = (root / name).read_text().splitlines()
        start = next(i for i, l in enumerate(text) if l.startswith(first))
        # The comment above the function comes with it.
        while start > 0 and text[start - 1].startswith(("//", "/*", " *")):
            start -= 1
        end = next(i for i in range(start + 1, len(text)) if re.match(last, text[i]))
        lines.append(f"/* {name} */")
        lines += text[start:end + 1]
        lines.append("")
    return [l.replace("\t", "    ")[:72] for l in lines]


def vt102demo():
    """A title, then the code that makes it scroll, scrolling."""
    title, sub = "Picocomputer 6502", "VT102 Terminal Demonstration"
    top, bot = 6, 24
    s = RESET + HIDE + sgr(0) + CSI + "2J"
    s += at(1, 1 + (40 - len(title)) // 2) + ESC + "#3" + sgr(1, 3, 38, 5, 213) + title
    s += at(2, 1 + (40 - len(title)) // 2) + ESC + "#4" + sgr(1, 3, 38, 5, 213) + title
    s += sgr(0) + at(3, 1 + (40 - len(sub)) // 2) + ESC + "#6" + sgr(4, 38, 5, 228) + sub
    s += sgr(0) + sgr(38, 5, 240) + DEC + at(top - 1) + "q" * 80 + at(bot + 1) + "q" * 80 + ASCII
    info = [
        sgr(38, 5, 117) + "Smooth scroll (DECSCLM, CSI ? 4 h) moves only the rows between the",
        sgr(38, 5, 117) + "margins, one scanline every frame, the way a DEC VT102 did in 1983.",
        sgr(38, 5, 252) + "Written by Claude, an AI model made by Anthropic, for the Picocomputer.",
        sgr(1, 38, 5, 213) + "Hi Mom!" + sgr(0, 38, 5, 245) + "  (Hello, Anthropic. I made something scroll.)",
    ]
    for i, text in enumerate(info):
        s += at(bot + 2 + i, 3) + text + sgr(0)
    s += region(top, bot) + SMOOTH + at(bot)
    code = listing()
    typo_line = next(i for i, l in enumerate(code) if "smooth_released" in l)
    for n, line in enumerate(code):
        typo = None
        if n == typo_line:
            line = line.replace("smooth_released", "smooth_relesaed")
            typo = "smooth_relesaed"
        s += "\n\r" + sgr(38, 5, 239) + f"{n + 1:4} " + c_colors(line, typo)
        if typo:
            col = 5 + line.index(typo)
            s += ("\n\r" + sgr(1, 38, 5, 196) + " " * col + "^" + "~" * (len(typo) - 1)
                  + " error: no member named '" + typo + "'" + sgr(0))
    s += "\n\r" * (bot - top + 1)
    return s


def rom_vt102demo():
    return player(vt102demo())


def ending():
    """Lines that scroll the whole screen, then the end of the program."""
    return RESET + SMOOTH + "".join(f"line {n}\r\n" for n in range(40))


def rom_ending():
    return player(ending(), loop=False)


def rom_ending_sync():
    return player(ending(), loop=False, sync=True)


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
        "smooth_forty": rom_forty, "vt102demo": rom_vt102demo,
        "smooth_ending": rom_ending, "smooth_ending_sync": rom_ending_sync}
TESTS = {"smooth_paced": test_paced}
main(globals())
