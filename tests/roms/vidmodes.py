#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Generates the video-mode mini-ROMs: one canvas, one mode program, one
# deterministic image, then STP. Every depth and canvas geometry gets a
# file. The emulator suite boots them and asserts a settled, non-empty
# picture; the FPGA suite runs the same files on both machines and demands
# pixel equality, so these ROMs are the shared corpus that keeps the two
# implementations honest. Committed alongside their generator; rerun it
# only to change the corpus.

import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parent


def record(addr, data):
    line = f"${addr:05X} ${len(data):X} ${zlib.crc32(bytes(data)) & 0xFFFFFFFF:08X}\n"
    return line.encode() + bytes(data)


def prog_mode(canvas, mode, attr, config_ptr):
    p = bytearray()

    def lda(v):
        p.extend((0xA9, v & 0xFF))

    def sta(a):
        p.extend((0x8D, a & 0xFF, a >> 8))

    def push(v):
        lda(v)
        sta(0xFFEC)

    def pushw(w):
        push(w >> 8)
        push(w & 0xFF)

    def op1():
        lda(0x01)
        sta(0xFFEF)
        p.extend((0x20, 0xF1, 0xFF))

    push(1), push(0), push(0), pushw(canvas)
    op1()
    push(1), push(0), push(1)
    pushw(mode), pushw(attr), pushw(config_ptr)
    pushw(0), pushw(0), pushw(0)  # plane 0, whole canvas
    op1()
    p.append(0xDB)
    return p


def rom(name, canvas, mode, attr, config, xram_chunks):
    body = b"#!RP6502\n"
    body += record(0x0300, prog_mode(canvas, mode, attr, 0x0100))
    body += record(0xFFFC, b"\x00\x03")
    body += record(0x10100, config)
    for addr, data in xram_chunks:
        body += record(0x10000 + addr, data)
    (OUT / f"{name}.rp6502").write_bytes(body)


def le16(*vals):
    b = bytearray()
    for v in vals:
        b.extend(((v & 0xFFFF) & 0xFF, (v & 0xFFFF) >> 8))
    return b


def mode3(name, canvas, attr, bpp, w, h, x, y, xram_pal):
    pal_ptr = 0x0200 if xram_pal else 0xFFFF
    data_ptr = 0x0800
    cfg = bytearray((0, 0)) + le16(x, y, w, h, data_ptr, pal_ptr)
    bm = bytes((i * 13 + 7) & 0xFF
               for i in range(((w * bpp + 7) // 8) * h))
    chunks = [(data_ptr, bm)]
    if xram_pal:
        chunks.append((0x0200, le16(*((0x0020 | (i * 2657))
                                      for i in range(1 << bpp)))))
    rom(name, canvas, 3, attr, cfg, chunks)


def mode1(name, canvas, attr, wchars, hchars, x, y, xram_pal, xram_font):
    fmt = attr & 7
    fh = 16 if attr & 8 else 8
    pal_ptr = 0x0200 if xram_pal else 0xFFFF
    font_ptr = 0x4000 if xram_font else 0xFFFF
    data_ptr = 0x0800
    cfg = bytearray((0, 0)) + le16(x, y, wchars, hchars, data_ptr, pal_ptr,
                                   font_ptr)
    cells = bytearray()
    for i in range(wchars * hchars):
        glyph = ord("A") + i % 60
        if fmt == 0:
            cells.append(glyph)
        elif fmt in (1, 2):
            cells.extend((glyph, (i * 3 + 1) & 0xFF))
        elif fmt == 3:
            cells.extend((glyph, (i * 5 + 1) & 0xFF, (i * 11 + 2) & 0xFF))
        else:
            cells.append(glyph)
            cells.append(i & 0xFF)  # reserved, ignored
            cells += le16(0x0020 | (i * 3141), 0x0020 | (i * 2718 + 9))
    chunks = [(data_ptr, cells)]
    if xram_pal:
        entries = 2 if fmt == 0 else (256 if fmt == 3 else 16)
        chunks.append((0x0200, le16(*((0x0020 | (i * 2657 + 5))
                                      for i in range(entries)))))
    if xram_font:
        chunks.append((0x4000, bytes((i * 7 + 3) & 0xFF
                                     for i in range(256 * fh))))
    rom(name, canvas, 1, attr, cfg, chunks)


mode3("mode3_8bpp", 3, 3, 8, 64, 64, 10, 20, True)
mode3("mode3_1bpp", 1, 0, 1, 64, 48, 5, 7, False)
mode3("mode3_4bppr", 2, 10, 4, 40, 30, 0, 0, False)
mode3("mode3_16bpp", 4, 4, 16, 32, 16, 100, 50, False)

mode1("mode1_1bpp8x8", 3, 0, 30, 12, 4, 6, False, False)
mode1("mode1_4bpp8x16", 1, 10, 20, 8, 8, 5, True, False)
mode1("mode1_4bppr8x8", 2, 1, 24, 10, 0, 0, False, False)
mode1("mode1_8bpp8x8", 1, 3, 16, 9, 3, 2, True, True)
mode1("mode1_16bpp8x16", 4, 12, 12, 6, 40, 30, False, False)
