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
# The file is the bench's to place; this only reads it.

import argparse

from rp6502_rom import (API_A, OP_CLOSE, OP_OPEN, OP_READ_XSTACK, O_RDONLY,
                        XSTACK, Asm, image)

NAME = "M.DAT"
CHUNK = 16
DONE = b"stream ok\r\n"

HANDLE = 0x0200


def prog():
    p = Asm()

    p.push_str(NAME)
    p.lda(O_RDONLY)
    p.sta(API_A)
    p.call(OP_OPEN)
    p.sta(HANDLE)

    # Read CHUNK bytes, print what came back, and go round until a read
    # answers with none — which is the end of the file and nothing else,
    # because a short read is still a read.
    loop = p.here()
    p.push(0)
    p.push(CHUNK)
    p.lda_abs(HANDLE)
    p.sta(API_A)
    p.call(OP_READ_XSTACK)

    p.emit(0xAA)  # tax — bytes read
    done = p.branch(0xF0)  # beq done

    inner = p.here()
    p.lda_abs(XSTACK)
    p.putc_a()
    p.dex()
    p.emit(0xD0, (inner - (p.here() + 2)) & 0xFF)  # bne inner
    p.jmp(loop)

    p.close(done)
    p.lda_abs(HANDLE)
    p.sta(API_A)
    p.call(OP_CLOSE)
    for c in DONE:
        p.lda(c)
        p.putc_a()
    p.stp()
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    ap.add_argument("--print-done", action="store_true")
    a = ap.parse_args()
    if a.print_done:
        print(DONE.decode(), end="")
    if a.emit:
        print(f"stream.rp6502 {image(prog()).write(a.emit)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
