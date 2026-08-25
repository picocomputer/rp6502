#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Generates the video-mode mini-ROMs: one canvas, one or more mode
# programs, one deterministic image, then STP. Every depth and canvas
# geometry gets a file, and one composite stacks all three planes. The
# emulator suite boots them and asserts a settled, non-empty picture; the
# FPGA suite runs the same files on both machines and demands pixel
# equality, so these ROMs are the shared corpus that keeps the two
# implementations honest.
#
# Output is a build artifact, not a commit. Every byte here is derived from
# this file, so committing the results only makes a second copy that can
# disagree with the generator. The .rp6502 files that remain in tests/roms
# are real cc65-built programs with no generator, which is why they stay.

import argparse
import sys
from pathlib import Path

# The assembler and the container live with the other test generators;
# this one lives beside the corpus it writes.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gen"))
from rp6502_asm import Asm  # noqa: E402
from rp6502_rom import Rom  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("--out", type=Path, required=True,
                help="directory to write the corpus into")
ap.add_argument("--emit-manifest", type=Path,
                help="one line per ROM: name width height")
ARGS = ap.parse_args()
OUT = ARGS.out
OUT.mkdir(parents=True, exist_ok=True)

# What each canvas selection means in pixels, which is the one thing a suite
# has to know about a fixture it did not write. The corpus is generated and a
# reader cannot enumerate it, so it says so itself rather than being copied
# into every suite that boots it.
CANVAS_SIZE = {0: (640, 480), 1: (320, 240), 2: (320, 180),
               3: (640, 480), 4: (640, 360)}
MANIFEST = []


def note(name, canvas):
    MANIFEST.append((name, *CANVAS_SIZE[canvas]))


def say(p, text):
    # Print through the console before anything else: LDA the byte, spin
    # on the TX-ready bit, store it. The terminal model keeps the text
    # across canvas switches, so a mode-0 slot shows it anywhere.
    for ch in text.encode("latin-1"):
        wait = p.local("say")
        p.lda_imm(ch)
        p.symbol(wait)
        p.bit_abs(0xFFE0)
        p.bpl(wait)
        p.sta_abs(0xFFE1)


def prog(canvas, progs, speak=None, stop=True):
    p = Asm()
    if speak:
        say(p, speak)
    p.xreg(1, 0, 0, canvas)
    for words in progs:
        p.xreg(1, 0, 1, *words)
    if stop:
        p.stp()
    return p


def rom(name, canvas, progs, xram_chunks, speak=None):
    r = Rom().program(prog(canvas, progs, speak))
    for addr, data in xram_chunks:
        r.record(0x10000 + addr, data)
    note(name, canvas)
    r.write(OUT / f"{name}.rp6502")


def le16(*vals):
    b = bytearray()
    for v in vals:
        b.extend(((v & 0xFFFF) & 0xFF, (v & 0xFFFF) >> 8))
    return b


def mode3(name, canvas, attr, bpp, w, h, x, y, xram_pal,
          x_wrap=False, y_wrap=False, config_ptr=0x0100, pal_ptr=0x0200):
    if not xram_pal:
        pal_ptr = 0xFFFF
    data_ptr = 0x0800
    cfg = bytearray((1 if x_wrap else 0, 1 if y_wrap else 0)) \
        + le16(x, y, w, h, data_ptr, pal_ptr)
    bm = bytes((i * 13 + 7) & 0xFF
               for i in range(((w * bpp + 7) // 8) * h))
    chunks = [(config_ptr, cfg), (data_ptr, bm)]
    if xram_pal:
        chunks.append((pal_ptr, le16(*((0x0020 | (i * 2657))
                                       for i in range(1 << bpp)))))
    rom(name, canvas, [(3, attr, config_ptr, 0, 0, 0)], chunks)


def mode1(name, canvas, attr, wchars, hchars, x, y, xram_pal, xram_font,
          x_wrap=False, y_wrap=False, pal_ptr=0x0200, config_ptr=0x0100):
    fmt = attr & 7
    fh = 16 if attr & 8 else 8
    if not xram_pal:
        pal_ptr = 0xFFFF
    font_ptr = 0x4000 if xram_font else 0xFFFF
    data_ptr = 0x0800
    cfg = bytearray((1 if x_wrap else 0, 1 if y_wrap else 0)) \
        + le16(x, y, wchars, hchars, data_ptr, pal_ptr, font_ptr)
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
    chunks = [(config_ptr, cfg), (data_ptr, cells)]
    if xram_pal:
        entries = 2 if fmt == 0 else (256 if fmt == 3 else 16)
        chunks.append((pal_ptr, le16(*((0x0020 | (i * 2657 + 5))
                                       for i in range(entries)))))
    if xram_font:
        chunks.append((0x4000, bytes((i * 7 + 3) & 0xFF
                                     for i in range(256 * fh))))
    rom(name, canvas, [(1, attr, config_ptr, 0, 0, 0)], chunks)


def mode2(name, canvas, attr, wt, ht, x, y, x_wrap, y_wrap, xram_pal,
          pal_ptr=0x0200):
    bpp = 1 << (attr & 3)
    tile_size = 16 if attr & 8 else 8
    n_tiles = 6
    if not xram_pal:
        pal_ptr = 0xFFFF
    data_ptr = 0x0800
    tile_ptr = 0x4000
    cfg = bytearray((1 if x_wrap else 0, 1 if y_wrap else 0)) \
        + le16(x, y, wt, ht, data_ptr, pal_ptr, tile_ptr)
    tmap = bytes((i * 5 + 2) % n_tiles for i in range(wt * ht))
    mem_size = tile_size * bpp // 8 * tile_size
    tiles = bytes((t // mem_size * 31 + t % mem_size * 7 + 3) & 0xFF
                  for t in range(n_tiles * mem_size))
    chunks = [(0x0100, cfg), (data_ptr, tmap), (tile_ptr, tiles)]
    if xram_pal:
        chunks.append((pal_ptr, le16(*((0x0020 | (i * 2657))
                                      for i in range(1 << bpp)))))
    rom(name, canvas, [(2, attr, 0x0100, 0, 0, 0)], chunks)


def mode5(name, canvas, attr, plane, sprites, n_pals=1,
          extra_progs=(), extra_chunks=(), desc_ptr=0x0100):
    # sprites: (x, y, image_index, palette) with palette an index, None
    # for the builtin, 'odd' for a misaligned pointer that falls back to
    # the builtin, or 'half' for a halfword-aligned read into the pool.
    bpp = 1 << (attr & 3)
    size = 8 << ((attr >> 3) & 7)
    dsize = size * (size * bpp // 8)
    img_base = 0x4000
    # Clear of the descriptor array, which is eight bytes a
    # sprite and runs past $0200 once a fixture asks for more
    # than thirty-two of them.
    pal_base = 0x0400
    cfg = bytearray()
    for x, y, im, ps in sprites:
        if ps is None:
            pptr = 0xFFFF
        elif ps == "odd":
            pptr = pal_base + 1
        elif ps == "half":
            pptr = pal_base + 2
        else:
            pptr = pal_base + ps * 0x400
        cfg += le16(x, y, img_base + im * dsize, pptr)
    chunks = [(desc_ptr, cfg)]
    for im in range(max(s[2] for s in sprites) + 1):
        chunks.append((img_base + im * dsize,
                       bytes((im * 47 + t * 13 + 5) & 0xFF
                             for t in range(dsize))))
    for pn in range(n_pals):
        chunks.append((pal_base + pn * 0x400,
                       le16(0, *((0x0020 | ((i * 2657 + pn * 97) & 0xFFFF))
                                 for i in range(1, 1 << bpp)))))
    chunks += list(extra_chunks)
    rom(name, canvas,
        list(extra_progs)
        + [(5, attr, desc_ptr, len(sprites), plane, 0, 0)],
        chunks)


def mode4(name, canvas, plane, log_size, sprites,
          extra_progs=(), extra_chunks=()):
    # sprites: (x, y, image_index, has_metadata). Every image carries a
    # metadata block — a word per row, sparse spans on even rows and
    # continuous full rows on odd — read only by the sprites that ask.
    size = 1 << log_size
    stride = size * size * 2 + size * 4
    # Above the mode-3 fills these fixtures stage at $2000, the
    # largest of which is 15,000 bytes: images at $4000 were being
    # overwritten by the bitmap that loaded after them.
    img_base = 0x6000
    cfg = bytearray()
    for x, y, im, meta in sprites:
        cfg += le16(x, y, img_base + im * stride)
        cfg += bytes((log_size, 1 if meta else 0))
    chunks = [(0x0100, cfg)]
    for im in range(max(s[2] for s in sprites) + 1):
        img = le16(*(((im * 47 + t * 13 + 5) & 0xFFFF)
                     for t in range(size * size)))
        meta = bytearray()
        for r in range(size):
            if r & 1:
                word = (1 << 31) | size
            else:
                word = (2 << 16) | (size - 2)
            meta += word.to_bytes(4, "little")
        chunks.append((img_base + im * stride, img + meta))
    chunks += list(extra_chunks)
    rom(name, canvas,
        list(extra_progs) + [(4, 0, 0x0100, len(sprites), plane, 0, 0)],
        chunks)


def mode4a(name, canvas, plane, log_size, sprites,
           extra_progs=(), extra_chunks=()):
    # sprites: (transform, x, y, image_index) — transform the six 8.8
    # matrix words {a00, a01, b0, a10, a11, b1}.
    size = 1 << log_size
    stride = size * size * 2
    # Above the mode-3 fills these fixtures stage at $2000, the
    # largest of which is 15,000 bytes: images at $4000 were being
    # overwritten by the bitmap that loaded after them.
    img_base = 0x6000
    cfg = bytearray()
    for tr, x, y, im in sprites:
        cfg += le16(*tr)
        cfg += le16(x, y, img_base + im * stride)
        cfg += bytes((log_size, 0))
    chunks = [(0x0100, cfg)]
    for im in range(max(s[3] for s in sprites) + 1):
        chunks.append((img_base + im * stride,
                       le16(*(((im * 47 + t * 13 + 5) & 0xFFFF)
                              for t in range(size * size)))))
    chunks += list(extra_chunks)
    rom(name, canvas,
        list(extra_progs) + [(4, 1, 0x0100, len(sprites), plane, 0, 0)],
        chunks)


def composite(name):
    # Plane 0: a mode 3 8bpp bitmap, the opaque base. Plane 1: mode 2
    # 1bpp tiles over it, color_2's transparent zero showing the base
    # through the glyph gaps. Plane 2: mode 1 raw-color cells, alpha set
    # on the foreground only.
    cfg3 = bytearray((0, 0)) + le16(10, 20, 200, 100, 0x0800, 0xFFFF)
    bm = bytes((i * 13 + 7) & 0xFF for i in range(200 * 100))
    cfg2 = bytearray((0, 0)) + le16(60, 50, 20, 8, 0x6000, 0xFFFF, 0x6100)
    tmap = bytes((i * 3 + 1) % 4 for i in range(20 * 8))
    tiles = bytes((t * 37 + 5) & 0xFF for t in range(4 * 8))
    cfg1 = bytearray((0, 0)) + le16(80, 60, 10, 3, 0x6800, 0xFFFF, 0xFFFF)
    cells = bytearray()
    for i in range(30):
        cells.append(ord("A") + i % 60)
        cells.append(i & 0xFF)  # reserved, ignored
        cells += le16(0x0020 | (i * 3141), (i * 2718 + 9) & 0xFFDF)
    rom(name, 1,
        [(3, 3, 0x0100, 0, 0, 0), (2, 0, 0x0110, 1, 0, 0),
         (1, 12, 0x0120, 2, 0, 0)],
        [(0x0100, cfg3), (0x0110, cfg2), (0x0120, cfg1),
         (0x0800, bm), (0x6000, tmap), (0x6100, tiles), (0x6800, cells)])


mode3("mode3_8bpp", 3, 3, 8, 64, 64, 10, 20, True)
mode3("mode3_1bpp", 1, 0, 1, 64, 48, 5, 7, False)
mode3("mode3_4bppr", 2, 10, 4, 40, 30, 0, 0, False)
mode3("mode3_16bpp", 4, 4, 16, 32, 16, 100, 50, False)
# The rest of the depth matrix, a halfword-aligned config and palette,
# content below row 180 on the undoubled 640x360 canvas, wraparound with
# negative positions, and both pointers at their exact XRAM bounds. 0xFFF2
# puts the config's fifth fetch past the top of XRAM: it wraps to word zero
# and lands in a slot mode 3 does not read.
mode3("mode3_2bpp", 1, 1, 2, 80, 60, 3, 5, True,
      config_ptr=0x0102, pal_ptr=0x0202)
mode3("mode3_4bpp", 3, 2, 4, 100, 80, 17, 9, False)
mode3("mode3_1bppr", 2, 8, 1, 64, 40, 7, 3, False)
mode3("mode3_2bppr", 4, 9, 2, 90, 50, 30, 200, False, config_ptr=0xFFF2)
mode3("mode3_wrap", 1, 3, 8, 50, 40, -37, -23, True,
      x_wrap=True, y_wrap=True, config_ptr=0xFDE2, pal_ptr=0xFE00)

mode1("mode1_1bpp8x8", 3, 0, 30, 12, 4, 6, False, False)
mode1("mode1_4bpp8x16", 1, 10, 20, 8, 8, 5, True, False)
mode1("mode1_4bppr8x8", 2, 1, 24, 10, 0, 0, False, False)
mode1("mode1_8bpp8x8", 1, 3, 16, 9, 3, 2, True, True, config_ptr=0x0102)
mode1("mode1_16bpp8x16", 4, 12, 12, 6, 40, 30, False, False)
# Wraparound with a mid-cell window entry, halfword-aligned palette.
mode1("mode1_wrap", 1, 0, 20, 6, -13, -9, True,
      False, x_wrap=True, y_wrap=True, pal_ptr=0x0202)

mode2("mode2_1bpp8", 3, 0x000, 60, 40, 80, 60, False, False, False)
mode2("mode2_2bpp16", 1, 0x009, 12, 8, -15, -10, False, False, True)
mode2("mode2_4bpp8trim", 1, 0x232, 20, 10, -7, -9, False, False, True)
mode2("mode2_8bpp16wrap", 2, 0x00B, 12, 8, 60, 20, True, True, True)
# Trim across the rest of the matrix: 16px tiles trimmed on both axes
# with a halfword-aligned palette, x-only at 4bpp, x-only at 8bpp, and
# y-only at 1bpp.
mode2("mode2_16trim", 1, 0x359, 10, 6, 4, 2, False, False, True,
      pal_ptr=0x0206)
mode2("mode2_trimx", 3, 0x022, 30, 12, 12, 20, False, False, True)
mode2("mode2_trimx8", 2, 0x013, 20, 8, 100, 50, False, False, True)
mode2("mode2_trimy", 2, 0x500, 24, 14, 6, 1, False, False, False)
composite("mode2_composite")

# Mode 5: a sprite-only plane (the zeroed claimed layer), sprites over a
# fill on the same plane, sprites under a text plane above, and the big
# squares on the letterboxed canvas from a non-zero plane — with clips
# off every edge, overlap, an offscreen skip, per-sprite palettes, the
# odd-pointer builtin fallback, and a halfword-aligned palette read.
mode5("mode5_8x8", 1, 3, 0, [
    (10, 20, 0, 0), (14, 24, 1, 0),
    (-3, 60, 0, 0), (314, 90, 1, 0),
    (100, -4, 0, 0), (200, 236, 1, 0),
    (400, 50, 0, None),
    (40, 100, 0, "odd"),
])
mode5("mode5_16x16", 1, 9, 0, [
    (30, 40, 0, 0), (60, 50, 1, "half"), (90, 60, 0, 0),
], extra_progs=[(3, 3, 0x01A0, 0, 0, 0)],
    extra_chunks=[
        (0x01A0, bytearray((0, 0)) + le16(20, 30, 80, 60, 0x2000, 0xFFFF)),
        (0x2000, bytes((i * 13 + 7) & 0xFF for i in range(80 * 60))),
])
mode5("mode5_32x32", 3, 18, 0, [
    (50, 50, 0, 0), (300, 120, 1, 1), (620, 200, 0, 0),
], n_pals=2,
    extra_progs=[(1, 0, 0x01A0, 2, 0, 0)],
    extra_chunks=[
        (0x01A0, bytearray((0, 0))
         + le16(250, 100, 12, 4, 0x2000, 0xFFFF, 0xFFFF)),
        (0x2000, bytes(ord("A") + i % 60 for i in range(12 * 4))),
])
mode5("mode5_64x64", 2, 27, 1, [(-20, 100, 0, 0), (280, 150, 0, 0)])

# Mode 4: raw sixteen-bit sprites — alpha-gated texels, opacity
# metadata's narrowed and continuous rows, clips off every edge, the
# same foreground walk from sprite-only, over-fill, and cross-plane
# slots.
mode4("mode4_8", 1, 0, 3, [
    (12, 22, 0, False), (16, 26, 1, False),
    (-5, 70, 0, False), (315, 100, 1, False),
    (150, -3, 0, False), (90, 234, 1, False),
    (500, 10, 0, False),
])
mode4("mode4_meta16", 1, 0, 4, [
    (30, 40, 0, True), (70, 60, 1, True), (-6, 90, 0, True),
], extra_progs=[(3, 3, 0x01A0, 0, 0, 0)],
    extra_chunks=[
        (0x01A0, bytearray((0, 0)) + le16(20, 30, 80, 60, 0x2000, 0xFFFF)),
        (0x2000, bytes((i * 13 + 7) & 0xFF for i in range(80 * 60))),
])
mode4("mode4_32", 3, 1, 5, [
    (100, 80, 0, True), (620, 300, 1, False),
], extra_progs=[(3, 2, 0x01A0, 0, 0, 0)],
    extra_chunks=[
        (0x01A0, bytearray((0, 0)) + le16(60, 40, 200, 150, 0x2000, 0xFFFF)),
        (0x2000, bytes((i * 13 + 7) & 0xFF
                       for i in range((200 * 4 + 7) // 8 * 150))),
])
mode4("mode4_64", 2, 2, 6, [(-30, 60, 0, False), (270, 120, 0, True)])
# Affine: identity's off-by-one sampling, rotation and scale driving
# the out-of-texture mask, clips both sides, over a fill and without.
mode4a("mode4a_id", 1, 0, 4, [
    ((0x100, 0, 0, 0, 0x100, 0), 40, 50, 0),
])
mode4a("mode4a_rot", 1, 0, 5, [
    ((0x0DD, 0x080, 0x300, -0x080 & 0xFFFF, 0x0DD, 0x200), 60, 60, 0),
    ((0x080, 0, 0, 0, 0x080, 0), 150, 80, 1),
    ((0x200, 0, 0x300, 0, 0x200, 0x300), 240, 100, 0),
])
def stress(name):
    # Every engine at once on the wide canvas: a wrapped full-width
    # bitmap fill under six 64-pixel sprites and four paletted ones,
    # text on top — the documented budget, at which the overrun counter
    # must hold zero.
    cfg3 = bytearray((0, 1)) + le16(0, 0, 640, 24, 0x8000, 0xFFFF)
    bm = bytes((i * 13 + 7) & 0xFF for i in range(640 * 24))
    m4c = bytearray()
    for i in range(6):
        m4c += le16(i * 100, 20 + i, 0x4000) + bytes((6, 0))
    m5c = bytearray()
    for i in range(4):
        m5c += le16(50 + i * 150, 30 + i * 8, 0x6000, 0x0200)
    cfg1 = bytearray((0, 0)) + le16(250, 100, 12, 4, 0x0300, 0xFFFF,
                                    0xFFFF)
    cells = bytes(ord("A") + i % 60 for i in range(48))
    rom(name, 3,
        [(3, 3, 0x01A0, 0, 0, 0),
         (4, 0, 0x0100, 6, 0, 0, 0),
         (5, 18, 0x0140, 4, 1, 0, 0),
         (1, 0, 0x01C0, 2, 0, 0)],
        [(0x0100, m4c), (0x0140, m5c), (0x01A0, cfg3), (0x01C0, cfg1),
         (0x0200, le16(0, *((0x0020 | (i * 2657))
                            for i in range(1, 16)))),
         (0x0300, cells),
         (0x4000, le16(*(((t * 13 + 5) & 0xFFFF)
                         for t in range(64 * 64)))),
         (0x6000, bytes((t * 7 + 3) & 0xFF for t in range(512))),
         (0x8000, bm)])


mode4a("mode4a_clip", 3, 0, 4, [
    ((0x100, 0, 0, 0, 0x100, 0), -7, 30, 0),
    ((0x100, 0, 0, 0, 0x100, 0), 630, 90, 1),
], extra_progs=[(3, 3, 0x01A0, 0, 0, 0)],
    extra_chunks=[
        (0x01A0, bytearray((0, 0)) + le16(10, 20, 120, 100, 0x2000, 0xFFFF)),
        (0x2000, bytes((i * 13 + 7) & 0xFF for i in range(120 * 100))),
])
stress("sprite_stress")

# The review's dark paths: mode 5 at 1bpp and the big squares, halfword
# descriptor arrays in every engine, the whole mode 4 log range with the
# defined row of the 32-bit-wrap sizes, small and large affine squares,
# and a slot built to lose its race so the overrun counter shows it.
mode5("mode5_1bpp128", 1, 32, 0, [
    (10, 40, 0, 0), (200, -30, 0, 0), (-60, 100, 0, None),
], desc_ptr=0x0102)
mode5("mode5_4bpp256", 3, 42, 0, [(30, -60, 0, 0)])

d = bytearray()
d += le16(5, 5, 0x1000) + bytes((0, 0))
d += le16(6, 6, 0x1000) + bytes((0, 0))
d += le16(7, 7, 0x1000) + bytes((0, 0))
d += le16(100, 30, 0x4000) + bytes((7, 1))
d += le16(0, 239, 0x2000) + bytes((16, 0))
m7 = le16(*(((t * 13 + 5) & 0xFFFF) for t in range(128 * 128)))
m7meta = bytearray()
for r in range(128):
    m7meta += (((1 << 31) | 128) if r & 1
               else ((2 << 16) | 126)).to_bytes(4, "little")
rom("mode4_sizes", 1, [(4, 0, 0x0102, 5, 0, 0, 0)],
    [(0x0102, d),
     (0x1000, le16(0x1234 | 0x20, 0x0FF5)),
     (0x2000, le16(*(((t * 11 + 9) & 0xFFFF) for t in range(640)))),
     (0x4000, m7 + m7meta)])

d = bytearray()
d += le16(0x100, 0, 0, 0, 0x100, 0) + le16(30, 40, 0x1000)     + bytes((3, 0))
d += le16(0x080, 0, 0, 0, 0x080, 0) + le16(100, 60, 0x4000)     + bytes((6, 0))
rom("mode4a_sizes", 1, [(4, 1, 0x0102, 2, 0, 0, 0)],
    [(0x0102, d),
     (0x1000, le16(*(((t * 13 + 5) & 0xFFFF) for t in range(64)))),
     (0x4000, le16(*(((t * 7 + 3) & 0xFFFF) for t in range(4096))))])

mode5("sprite_overrun", 1, 27, 0,
      [(i * 6, 40, 0, 0) for i in range(48)])

# Mode 0 as a slot: the terminal over a mode-3 bitmap, its
# default-background cells transparent and its inked ones opaque, on
# every canvas geometry — the 80-column faces at 640 wide, the
# 40-column 8x8 faces at 320, DEC graphics on both. The script hides
# the cursor or the frames never settle.
MODE0_SAY = ("\33[0m\33[2J\33[H\33[?25l"
             "term over bitmap\r\n"
             "\33(0lqqqqk\33(B\r\n"
             "\33[43;34m opaque \33[0m done")


def mode0mix(name, canvas, plane, begin, end):
    cfg = bytearray((0, 0)) + le16(20, 10, 200, 150, 0x0800, 0xFFFF)
    rom(name, canvas,
        [(3, 3, 0x0100, 0, 0, 0), (0, plane, begin, end)],
        [(0x0100, cfg),
         (0x0800, bytes((i * 13 + 7) & 0xFF for i in range(200 * 150)))],
        speak=MODE0_SAY)


mode0mix("mode0_overlay", 3, 1, 32, 464)
mode0mix("mode0_win360", 4, 1, 0, 0)
mode0mix("mode0_win240", 1, 1, 0, 0)
mode0mix("mode0_win180", 2, 1, 0, 0)

# The graphics round trip home: a canvas, a fill, then the console
# again — the return must republish the vsync line and the console
# must still be whole.
p = prog(4, [(3, 3, 0x0100, 0, 0, 0)], speak=MODE0_SAY, stop=False)
p.xreg(1, 0, 0, 0)
p.stp()
r = Rom().program(p)
r.record(0x10000 + 0x0100,
         bytearray((0, 0)) + le16(20, 10, 200, 150, 0x0800, 0xFFFF))
r.record(0x10000 + 0x0800,
         bytes((i * 13 + 7) & 0xFF for i in range(200 * 150)))
# It ends on the console canvas, not the one it opened with, and what a
# suite measures is where it settled.
note("mode0_return", 0)
r.write(OUT / "mode0_return.rp6502")

# The serial fill canary: two 8bpp XRAM-palette fills — the costliest
# prologue — on the wide canvas, overlapping so the stacking shows,
# where the one engine runs them back-to-back against the beam.
cfg0 = bytearray((0, 0)) + le16(10, 20, 64, 64, 0x0800, 0x0200)
cfg1 = bytearray((0, 0)) + le16(40, 30, 64, 64, 0x1800, 0x0600)
rom("fill_heavy640", 3,
    [(3, 3, 0x0100, 0, 0, 0), (3, 3, 0x0140, 1, 0, 0)],
    [(0x0100, cfg0), (0x0140, cfg1),
     (0x0800, bytes((i * 13 + 7) & 0xFF for i in range(64 * 64))),
     (0x1800, bytes((i * 11 + 3) & 0xFF for i in range(64 * 64))),
     (0x0200, le16(*((0x0020 | (i * 2657)) for i in range(256)))),
     (0x0600, le16(*((0x0020 | (i * 1031 + 5)) for i in range(256))))])

if ARGS.emit_manifest:
    ARGS.emit_manifest.write_text(
        "".join(f"{n} {w} {h}\n" for n, w, h in sorted(MANIFEST)))
