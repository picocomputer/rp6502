#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Which roots does the host's Open File actually take? Five spellings,
# one file each:
#
#   0  000.bin                          the drive's own rooting
#   1  Assets/rp6502/common/001.bin     Assets, no leading slash
#   2  /Assets/rp6502/common/002.bin    Assets, rooted
#   3  Saves/rp6502/common/003.bin      Saves, no leading slash
#   4  /Saves/rp6502/common/004.bin     Saves, rooted
#
# The firmware passes names that spell a host root through verbatim,
# so what the host answers here is about the host, not about msc.c.
#
# Per file: open plainly and print E if it existed; if not, print -
# then create and print C, or X if the create was refused. A live
# descriptor gets eight bytes of its own index digit — so the card
# afterwards shows, in file sizes and contents, which spellings made
# real files and where — then W for a full write, w for anything less.
#
# A clean console reads like: 0:-CW 1:-CW 2:-CW 3:-CW 4:-CW
# with E in place of -C on files a previous run left behind.

import argparse
import zlib
from pathlib import Path

from bigfile_rom_gen import (ORG, XSTACK, API_A, API_OP, API_CALL, RIA_TX,
                             RIA_READY, OP_OPEN, OP_CLOSE,
                             OP_WRITE_XSTACK, O_RDONLY, O_WRONLY, O_CREAT,
                             O_TRUNC, Prog)

O_RDWR = 0x03

FD = 0x0200

NAMES = [
    "000.bin",
    "Assets/rp6502/common/001.bin",
    "/Assets/rp6502/common/002.bin",
    "Saves/rp6502/common/003.bin",
    "/Saves/rp6502/common/004.bin",
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

    main = p.here()
    p.b[jmp_main] = main & 0xFF
    p.b[jmp_main + 1] = main >> 8

    def text(s):
        for c in s.encode():
            p.lda(c)
            p.jsr(putc)

    def open_it(name, flags):
        p.push_str(name)
        p.store(API_A, flags)
        p.call(OP_OPEN)
        p.sta(FD)

    # Forward jumps, because the name pushes are far longer than a
    # branch reaches: emit jmp $0000 and patch the target in later.
    def fjmp():
        p.emit(0x4C, 0x00, 0x00)
        return len(p.b) - 2

    def fpatch(h):
        t = p.here()
        p.b[h] = t & 0xFF
        p.b[h + 1] = t >> 8

    for i, name in enumerate(NAMES):
        text(f"{i}:")

        open_it(name, O_RDWR)
        p.lda_abs(FD)
        p.emit(0xC9, 0xFF)  # cmp #$FF
        missing = p.branch(0xF0)
        text("E")
        j_have = fjmp()  # to the write, fd in hand
        p.close(missing)
        text("-")
        open_it(name, O_RDWR | O_CREAT | O_TRUNC)
        p.lda_abs(FD)
        p.emit(0xC9, 0xFF)
        made = p.branch(0xD0)
        text("X ")
        j_skip = fjmp()  # nothing to write to
        p.close(made)
        text("C")
        fpatch(j_have)

        # Eight bytes of the index digit, then the write's own count.
        for _ in range(8):
            p.push(ord("0") + i)
        p.lda_abs(FD)
        p.sta(API_A)
        p.call(OP_WRITE_XSTACK)
        p.emit(0xC9, 0x08)  # cmp #8
        short = p.branch(0xD0)
        text("W")
        j_wrote = fjmp()
        p.close(short)
        text("w")
        fpatch(j_wrote)
        p.lda_abs(FD)
        p.sta(API_A)
        p.call(OP_CLOSE)
        text(" ")

        fpatch(j_skip)

    text("\r\nDONE\r\n")
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
        print(f"roots.rp6502 {len(rom)} bytes, five root spellings")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
