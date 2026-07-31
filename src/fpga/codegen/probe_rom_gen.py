#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Creating a file has never worked on hardware. Open File with bit 0 set
# answers 3, "file not found", against a name that is not already there,
# while opening one that is works. Everything we send matches the
# documented struct: the path is rooted at a platform folder core.json
# declares, the slot is not read-only, the flags are at 0x100 and the
# size at 0x104, and the path parses — a malformed one answers 4, not 3.
#
# So the remaining variables are in the name itself, and each is a guess
# no document settles: whether the extension is matched against the
# slot's lowercase list case-sensitively, whether an extension is needed
# at all, and whether a subdirectory in the leaf changes the answer.
#
# A .rp6502 is a card file, so this costs a copy rather than a fit. Each
# attempt prints its own line, and msc.c prints "msc: open rc=N" ahead of
# any that failed, so a line with no rc above it is the one that worked.

import argparse
import zlib
from pathlib import Path

from bigfile_rom_gen import (ORG, API_A, API_OP, API_CALL, RIA_TX, RIA_READY,
                             OP_OPEN, OP_CLOSE, O_RDONLY, O_WRONLY, O_CREAT,
                             O_TRUNC, Prog)

FD = 0x0200

# The first asks whether last run's file is still there, so the rest are
# read against a known card rather than an assumed one. The others each
# change exactly one thing about the name.
CASES = [
    ("0", "T2.DAT", O_RDONLY),
    ("1", "T2.DAT", O_WRONLY | O_CREAT | O_TRUNC),
    ("2", "t2.dat", O_WRONLY | O_CREAT | O_TRUNC),
    ("3", "T2.SAV", O_WRONLY | O_CREAT | O_TRUNC),
    ("4", "t2.sav", O_WRONLY | O_CREAT | O_TRUNC),
    ("5", "t2.bin", O_WRONLY | O_CREAT | O_TRUNC),
    ("6", "t2.txt", O_WRONLY | O_CREAT | O_TRUNC),
    ("7", "T2", O_WRONLY | O_CREAT | O_TRUNC),
    # Create without truncate: if the host refuses to make a zero-byte
    # file, this is the one that separates that from the flag itself.
    ("8", "t2c.dat", O_WRONLY | O_CREAT),
]


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

    for tag, name, flags in CASES:
        p.push_str(name)
        p.store(API_A, flags)
        p.call(OP_OPEN)
        p.sta(FD)
        text(tag + "=")
        p.lda_abs(FD)
        p.jsr(puthex)
        text("\r\n")
        # FF is the failure, and closing it would free a descriptor that
        # was never taken.
        p.lda_abs(FD)
        p.emit(0xC9, 0xFF)  # cmp #$FF
        skip = p.branch(0xF0)  # beq
        p.lda_abs(FD)
        p.sta(API_A)
        p.call(OP_CLOSE)
        p.close(skip)

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
        print(f"probe.rp6502 {len(rom)} bytes, {len(CASES)} cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
