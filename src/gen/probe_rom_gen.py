#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Can the host hold a file at zero bytes?
#
# It looked settled that it could not: every create ever attempted came
# back 3, "file not found", and a length of zero was the obvious reason.
# It was the wrong reason. The operation flags were being written low
# byte first into a stream the host reads as a word, so flags of 3 went
# out as 0x03000000 and the host saw no create bit and no resize bit at
# all. It opened what was there and refused what was not, which is what
# a missing create bit looks like from here.
#
# With the word the right way round a create takes and a shrink takes,
# both proven — the shrink being the one a write cannot counterfeit,
# since no write makes a file smaller. So the zero question was never
# actually asked, and the firmware now asks it plainly: create with no
# size, truncate to no size, and report what the host says the lengths
# are afterwards.
#
# Lengths come from lseek to the end rather than from a read, because a
# read returns what was asked for and a capped read reports its own cap.
# They are taken through a fresh open, because msc.c keeps its own idea
# of the length and would otherwise report the answer it hoped for.
#
# Needs a t2.bin in /Assets/rp6502/common/ and no n2.bin. Deleting the
# whole folder instead turns the same ROM into the other open question:
# whether the host creates the tree on its way to a file.

import argparse

from rp6502_rom import (API_A, ORG, OP_CLOSE, OP_LSEEK, OP_OPEN, O_CREAT,
                        O_RDONLY, O_TRUNC, O_WRONLY, RIA_READY, RIA_TX, Asm,
                        image)


# api_pop_int8 takes whence before api_pop_int32_end takes the offset, so
# whence is pushed last. cc65 spells END as 1, not 2.
OP_LSEEK = 0x1A
SEEK_END_CC65 = 1

FD = 0x0200
# The file to truncate, which must exist, and the name to create, which
# must not. Both end at zero if the host allows it, so use a new NEW
# each run or delete the last one.
OLD = "t2.bin"
NEW = "n2.bin"


def build():
    p = Asm()
    p.emit(0x4C, 0x00, 0x00)  # jmp main
    jmp_main = 1

    putc = p.here()
    p.emit(0x48)  # pha
    p.emit(0x2C, RIA_READY & 0xFF, RIA_READY >> 8)  # bit
    p.emit(0x10, 0xFB)  # bpl -5
    p.emit(0x68)  # pla
    p.sta(RIA_TX)
    p.rts()

    putnib = p.here()
    p.emit(0xC9, 0x0A)  # cmp #10
    p.emit(0xB0, 0x06)  # bcs letter
    p.emit(0x18)
    p.emit(0x69, ord("0"))
    p.jmp(putc)
    p.emit(0x18)
    p.emit(0x69, ord("A") - 10)
    p.jmp(putc)

    puthex = p.here()
    p.emit(0x48)  # pha
    p.emit(0x4A, 0x4A, 0x4A, 0x4A)  # lsr x4
    p.jsr(putnib)
    p.emit(0x68)  # pla
    p.emit(0x29, 0x0F)
    p.jmp(putnib)

    main = p.here()
    p.b[jmp_main] = main & 0xFF
    p.b[jmp_main + 1] = main >> 8

    def text(s):
        for c in s.encode():
            p.lda(c)
            p.jsr(putc)

    def open_name(name, flags):
        p.push_str(name)
        p.store(API_A, flags)
        p.call(OP_OPEN)
        p.sta(FD)

    def close_fd():
        p.lda_abs(FD)
        p.emit(0xC9, 0xFF)  # cmp #$FF
        skip = p.branch(0xF0)
        p.lda_abs(FD)
        p.sta(API_A)
        p.call(OP_CLOSE)
        p.close(skip)

    def show_len(tag, name):
        """Seek to the end and print the low sixteen bits of the length."""
        open_name(name, O_RDONLY)
        text(tag + "=")
        p.lda_abs(FD)
        p.jsr(puthex)
        text("/")
        for _ in range(4):
            p.push(0)  # a zero offset, so its byte order cannot matter
        p.push(SEEK_END_CC65)
        p.lda_abs(FD)
        p.sta(API_A)
        p.call(OP_LSEEK)
        # api_return_axsreg puts bits 7:0 in A and 15:8 in X.
        p.emit(0x48)  # pha
        p.emit(0x8A)  # txa
        p.jsr(puthex)
        p.emit(0x68)  # pla
        p.jsr(puthex)
        text("\r\n")
        close_fd()

    def attempt(tag, name, flags):
        open_name(name, flags)
        text(tag + "=")
        p.lda_abs(FD)
        p.jsr(puthex)
        text("\r\n")
        close_fd()

    show_len("L0", OLD)

    # A create that names no size at all.
    attempt("N", NEW, O_WRONLY | O_CREAT)
    show_len("L1", NEW)

    # A truncate to nothing, which is the same question from the other side.
    attempt("T", OLD, O_WRONLY | O_TRUNC)
    show_len("L2", OLD)

    text("DONE\r\n")
    p.emit(0xDB)  # stp
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    a = ap.parse_args()
    if a.emit:
        n = image(build()).write(a.emit)
        print(f"probe.rp6502 {n} bytes, shrink probe on {OLD}, create {NEW}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
