#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Creating a file has never worked on hardware, and neither the name nor
# the folder is why. Nine variants of extension, case, missing extension
# and create-without-truncate all answered 3, file not found, while a
# name already on the card opens and returns a descriptor. So the path,
# the folder, the slot binding and the first 256 bytes of the parameter
# struct are all proven good.
#
# What is not proven is that the operation flags at offset 0x100 arrive
# at all. A truncate of an existing file left every byte in place, but
# that measures nothing on its own: O_TRUNC resizes to zero, and a host
# that refuses any zero-length operation would look identical to a host
# that never saw the flags. Both also explain the create failure, since
# msc.c pairs create with resize-to-zero.
#
# An append does NOT separate them, which cost a wrong conclusion: the
# same call that asks for the resize also writes past the old end, so a
# host whose Slot Write extends on its own grows the file by exactly as
# much whether the resize bit was read or ignored.
#
# A SHRINK does separate them. No write can make a file smaller, so a
# file that comes back shorter than it was proves the host read the
# resize bit, and a bit that is read proves the create bit beside it in
# the same byte is read too. O_TRUNC now asks for exactly that: one
# byte, which is nonzero, on a file that is longer.
#
# The length is measured with lseek to the end, not by reading: a read
# returns what was asked for, so a capped read reports its own cap and
# says nothing about a file of any size. It is taken through a fresh
# open too, because msc.c keeps its own idea of the length and would
# report the answer it hoped for rather than the one the host gave.
#
# Needs a t2.bin in /Saves/rp6502/common/ longer than one byte, and no
# n1.bin. The run shortens t2.bin to a byte, so put a fresh one back
# before repeating.

import argparse
import zlib
from pathlib import Path

from bigfile_rom_gen import (ORG, API_A, API_OP, API_CALL, RIA_TX, RIA_READY,
                             OP_OPEN, OP_CLOSE, O_RDONLY, O_WRONLY, O_CREAT,
                             O_TRUNC, Prog)

# api_pop_int8 takes whence before api_pop_int32_end takes the offset, so
# whence is pushed last. cc65 spells END as 1, not 2.
OP_LSEEK = 0x1A
SEEK_END_CC65 = 1

FD = 0x0200
# The file to shrink, which must exist and be longer than one byte, and
# the name to create, which must NOT exist. The run destroys the first:
# put a fresh one back before repeating.
OLD = "t2.bin"
NEW = "n1.bin"


def build():
    p = Prog()
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

    # Does a create work now that it asks for a byte rather than nothing?
    attempt("N", NEW, O_WRONLY | O_CREAT)
    show_len("L1", NEW)

    # The discriminator: a nonzero resize that can only be a shrink.
    attempt("T", OLD, O_WRONLY | O_TRUNC)
    show_len("L2", OLD)

    text("DONE\r\n")
    p.emit(0xDB)  # stp
    return bytes(p.b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    a = ap.parse_args()
    if a.emit:
        body = build()
        rom = b"#!RP6502\n"
        for addr, data in ((ORG, body),
                           (0xFFFC, bytes((ORG & 0xFF, ORG >> 8)))):
            crc = zlib.crc32(data) & 0xFFFFFFFF
            rom += f"${addr:05X} ${len(data):X} ${crc:08X}\n".encode() + data
        Path(a.emit).write_bytes(rom)
        print(f"probe.rp6502 {len(rom)} bytes, shrink probe on {OLD}, create {NEW}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
