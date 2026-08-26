#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The console read as a device, which nothing was asking about.
#
# A program reads its input two ways. One is stdin, which the line editor
# owns and which adventure.txt and the line-editor suites walk end to end.
# The other is TTY:, opened by name, read raw, no editor in between -- and
# that path had no test at all. The filesystem ROM opens it and closes it
# again without ever reading a byte.
#
# So: open it, echo back whatever arrives, and stop on a byte agreed with
# the driver below. Nothing here is about the terminal or the layout; the
# claim is only that a byte typed at the machine reaches a program that
# asked the console for it.

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rp6502_script  # noqa: E402
from rp6502_asm import (API_A, OP_CLOSE, OP_OPEN, OP_READ_XSTACK, O_RDONLY,
                        XSTACK, Asm, putc)  # noqa: E402
from rp6502_rom import image  # noqa: E402

FD = 0x0200

# What the driver types, and the byte that ends the run. The terminator is
# printed like the rest, so the driver waits for the whole line and knows
# the program saw every byte before it stopped.
TYPED = "console reads"
END = "."


def prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc)

    p.symbol("main")
    p.push_str("TTY:")
    p.lda_imm(O_RDONLY)
    p.sta_abs(API_A)
    p.call(OP_OPEN)
    p.sta_abs(FD)

    # A raw console read answers with whatever is queued now, which is
    # nothing most times round: the program is faster than a person, and
    # the emulator's ring fills a frame at a time.
    p.symbol("poll")
    p.push(0)
    p.push(8)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)
    p.tax()
    with p.branch("beq"):
        p.symbol("echo")
        p.lda_abs(XSTACK)
        p.sta_abs(0x0201)
        p.jsr_abs("putc")
        p.lda_abs(0x0201)
        p.cmp_imm(ord(END))
        with p.branch("bne"):
            p.lda_abs(FD)
            p.sta_abs(API_A)
            p.call(OP_CLOSE)
            p.stp()
        p.dex()
        p.bne("echo")
    p.jmp_abs("poll")
    return p


def drive(emu, rom):
    """The other half: type at the machine, read it back.

    `type` is the seam a host keystroke enters through, and the program
    above is holding TTY: open -- so the text coming back out is proof
    that the raw console read delivered it."""
    def body(e):
        e.cmd("run 10")  # let the program reach its poll loop
        e.cmd(f'type "{TYPED}{END}"')
        e.cmd(f'wait "{TYPED}{END}"')
    return rp6502_script.drive(emu, rom, body)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--emit")
    ap.add_argument("--drive", action="store_true",
                    help="run the ROM on the emulator and type at it")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the .rp6502 --emit wrote")
    a = ap.parse_args()
    if a.emit:
        print(f"tty.rp6502 {image(prog()).write(a.emit)} bytes")
    if a.drive:
        return drive(a.emu, a.rom)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
