#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# A .rp6502 that holds a file open and reads it a chunk at a time,
# printing each chunk as it arrives. It exists for one question that
# nothing else asks: whether a file that is open when the machine sleeps
# is still open when it wakes.
#
# The card is the one thing a savestate cannot carry and cannot rebuild
# from itself — the host's binding of a data slot to a file belongs to
# the session the wake ended — so the interesting instant is a read that
# lands after the resume against a slot the firmware had to bind again.
# Reading in chunks is what puts one there wherever the sleep falls.
#
# The file is placed by whoever runs it: the Pocket's bench binds a card,
# and --drive below lays one down.

import argparse
import pathlib

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rp6502_script  # noqa: E402
from rp6502_asm import (API_A, OP_CLOSE, OP_OPEN, OP_READ_XSTACK, O_RDONLY,
                        XSTACK, Asm)
from rp6502_rom import image

NAME = "M.DAT"
CHUNK = 16
DONE = b"stream ok\r\n"

HANDLE = 0x0200


def prog():
    p = Asm()

    p.push_str(NAME)
    p.lda_imm(O_RDONLY)
    p.sta_abs(API_A)
    p.call(OP_OPEN)
    p.sta_abs(HANDLE)

    # Read CHUNK bytes, print what came back, and go round until a read
    # answers with none — which is the end of the file and nothing else,
    # because a short read is still a read.
    p.symbol("loop")
    p.push(0)
    p.push(CHUNK)
    p.lda_abs(HANDLE)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)

    p.tax()  # bytes read
    with p.branch("beq"):
        p.symbol("inner")
        p.lda_abs(XSTACK)
        p.putc_a()
        p.dex()
        p.bne("inner")
        p.jmp_abs("loop")

    p.lda_abs(HANDLE)
    p.sta_abs(API_A)
    p.call(OP_CLOSE)
    for c in DONE:
        p.lda_imm(c)
        p.putc_a()
    p.stp()
    return p


def drive(emu, rom):
    """The other half of this file: the program above, watched.

    The Pocket's bench asks whether the file survives a sleep, and answers
    it with a card the host binds. Here there is no card, so the driver
    lays the file down itself -- which also closes the hole that made the
    question askable at all: with nothing to open, the program reads none
    and prints the same DONE it prints on success. The payload spans
    several chunks, so the loop is walked rather than skipped."""
    # No zero byte: the console capture a `wait` searches is a C string,
    # so a NUL in the stream would hide everything printed after it.
    payload = bytes(1 + (i * 7 + 11) % 0xFF for i in range(CHUNK * 5))
    pathlib.Path(NAME).write_bytes(payload)

    def body(e):
        # The tail arrives only after every chunk before it did.
        e.cmd(f'wait "{DONE.decode().rstrip()}"')
    return rp6502_script.drive(emu, rom, body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    ap.add_argument("--drive", action="store_true",
                    help="run the ROM on the emulator and check what it says")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the .rp6502 --emit wrote")
    a = ap.parse_args()
    if a.emit:
        print(f"stream.rp6502 {image(prog()).write(a.emit)} bytes")
    if a.drive:
        return drive(a.emu, a.rom)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
