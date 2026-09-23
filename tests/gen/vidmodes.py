#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import sys
from pathlib import Path

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

CANVAS_SIZE = {0: (640, 480), 1: (320, 240), 2: (320, 180),
               3: (640, 480), 4: (640, 360)}
MANIFEST = []


def note(name, canvas):
    MANIFEST.append((name, *CANVAS_SIZE[canvas]))


def say(p, text):
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
          x_wrap=False, y_wrap=False, config_ptr=0x0100, pal_ptr=0x0200,
          data_ptr=0x0800):
    if not xram_pal:
        pal_ptr = 0xFFFF
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
            cells.append(i & 0xFF)  # VGA ignores the attributes byte.
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


MODE2_TILES = 6


def mode2_map(wt, ht):
    return bytes((i * 5 + 2) % MODE2_TILES for i in range(wt * ht))


def mode2_tiles(attr):
    bpp = 1 << (attr & 3)
    tile_size = 16 if attr & 8 else 8
    mem_size = tile_size * bpp // 8 * tile_size
    return bytes((t // mem_size * 31 + t % mem_size * 7 + 3) & 0xFF
                 for t in range(MODE2_TILES * mem_size))


def mode2(name, canvas, attr, wt, ht, x, y, x_wrap, y_wrap, xram_pal,
          pal_ptr=0x0200, tile_ptr=0x4000):
    bpp = 1 << (attr & 3)
    if not xram_pal:
        pal_ptr = 0xFFFF
    data_ptr = 0x0800
    cfg = bytearray((1 if x_wrap else 0, 1 if y_wrap else 0)) \
        + le16(x, y, wt, ht, data_ptr, pal_ptr, tile_ptr)
    tiles = mode2_tiles(attr)
    chunks = [(0x0100, cfg), (data_ptr, mode2_map(wt, ht))]
    # A tile set may run off the end of XRAM. The tiles that do are never
    # read, so only the part that fits is written.
    head = min(len(tiles), 0x10000 - tile_ptr)
    chunks.append((tile_ptr, tiles[:head]))
    if head < len(tiles):
        chunks.append((0, tiles[head:]))
    if xram_pal:
        chunks.append((pal_ptr, le16(*((0x0020 | (i * 2657))
                                      for i in range(1 << bpp)))))
    rom(name, canvas, [(2, attr, 0x0100, 0, 0, 0)], chunks)


def mode5(name, canvas, attr, plane, sprites, n_pals=1,
          extra_progs=(), extra_chunks=(), desc_ptr=0x0100):
    bpp = 1 << (attr & 3)
    size = 8 << ((attr >> 3) & 7)
    dsize = size * (size * bpp // 8)
    img_base = 0x4000
    # The palettes start at $0400, clear of the descriptors at $0100: eight
    # bytes a sprite here, ten in mode5c, which reach $0400 after 76 sprites.
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


MODE5_HFLIP = 0x10
MODE5_VFLIP = 0x20
MODE5_HDBL = 0x40
MODE5_VDBL = 0x80
MODE5_ALL4 = MODE5_HFLIP | MODE5_VFLIP | MODE5_HDBL | MODE5_VDBL


def mode5c_image(im, w, h, bpp):
    return bytes((im * 47 + t * 13 + 5) & 0xFF
                 for t in range((w * bpp + 7) // 8 * h))


def mode5c_palette(pn):
    return le16(0, *((0x0020 | ((i * 2657 + pn * 97) & 0xFFFF))
                     for i in range(1, 256)))


def mode5c(name, canvas, plane, images, sprites, n_pals=1, extra_progs=(),
           extra_chunks=(), desc_ptr=0x0100, attr=0x38, img_base=0x4000,
           pal_base=0x0400, data=()):
    # An image is (w, h, bpp) at the next free byte from img_base, or
    # (w, h, bpp, addr). A sprite's palette is None for the builtin one,
    # "odd" for a halfword pointer, a palette number, or ("at", pointer).
    # data maps an image number to its bytes in place of the formula.
    data = dict(data)
    assert desc_ptr >= 0x0800 or desc_ptr + 10 * len(sprites) <= pal_base
    chunks = []
    iptrs = []
    dims = []
    at = img_base
    for im, spec in enumerate(images):
        w, h, bpp = spec[:3]
        addr = spec[3] if len(spec) > 3 else at
        img = data[im] if im in data else mode5c_image(im, w, h, bpp)
        assert len(img) == (w * bpp + 7) // 8 * h
        if addr + len(img) <= 0x10000:
            chunks.append((addr, img))
        if len(spec) == 3:
            at += len(img)
        iptrs.append(addr)
        dims.append((w, h, bpp))
    cfg = bytearray()
    for x, y, im, ps, opt in sprites:
        if ps is None:
            pptr = 0xFFFF
        elif ps == "odd":
            pptr = pal_base + 1
        elif isinstance(ps, tuple):
            pptr = ps[1]
        else:
            pptr = pal_base + ps * 0x400
        w, h, bpp = dims[im]
        cfg += le16(x, y, iptrs[im], pptr)
        cfg += bytes((((h // 4 - 1) << 4) | (w // 4 - 1),
                      opt | (bpp.bit_length() - 1)))
    chunks.insert(0, (desc_ptr, cfg))
    for pn in range(n_pals):
        chunks.append((pal_base + pn * 0x400, mode5c_palette(pn)))
    chunks += list(extra_chunks)
    rom(name, canvas,
        list(extra_progs)
        + [(5, attr, desc_ptr, len(sprites), plane, 0, 0)],
        chunks)


def img_rows(img, w, h, bpp):
    bpr = (w * bpp + 7) // 8
    mask = (1 << bpp) - 1
    rows = []
    for r in range(h):
        bits = int.from_bytes(img[r * bpr:(r + 1) * bpr], "big")
        rows.append([(bits >> (bpr * 8 - (c + 1) * bpp)) & mask
                     for c in range(w)])
    return rows


def img_pack(rows, bpp):
    out = bytearray()
    for row in rows:
        bpr = (len(row) * bpp + 7) // 8
        bits = 0
        for v in row:
            bits = (bits << bpp) | v
        # The bits past the width in a row's last byte are set, so a
        # renderer that reads them draws opaque pixels and is caught.
        tail = bpr * 8 - len(row) * bpp
        bits = (bits << tail) | ((1 << tail) - 1)
        out += bits.to_bytes(bpr, "big")
    return bytes(out)


def img_hflip(rows):
    return [row[::-1] for row in rows]


def img_vflip(rows):
    return rows[::-1]


def img_hdbl(rows):
    return [[v for v in row for _ in (0, 1)] for row in rows]


def img_vdbl(rows):
    return [row for row in rows for _ in (0, 1)]


def mode5c_pair(name, canvas, plane, images, sprites, **kw):
    # <name> draws with the sprites' flags, and <name>_ref draws the same
    # picture from images transformed here, with the flags clear.
    mode5c(name, canvas, plane, images, sprites, **kw)
    ref_images = []
    ref_sprites = []
    data = {}
    made = {}
    for x, y, im, ps, opt in sprites:
        flags = opt & 0xF0
        if (im, flags) not in made:
            w, h, bpp = images[im][:3]
            rows = img_rows(mode5c_image(im, w, h, bpp), w, h, bpp)
            if flags & MODE5_HFLIP:
                rows = img_hflip(rows)
            if flags & MODE5_VFLIP:
                rows = img_vflip(rows)
            if flags & MODE5_HDBL:
                rows = img_hdbl(rows)
            if flags & MODE5_VDBL:
                rows = img_vdbl(rows)
            made[(im, flags)] = len(ref_images)
            data[len(ref_images)] = img_pack(rows, bpp)
            ref_images.append((len(rows[0]), len(rows), bpp))
        ref_sprites.append((x, y, made[(im, flags)], ps, opt & 0x0F))
    mode5c(name + "_ref", canvas, plane, ref_images, ref_sprites,
           data=data, **kw)


def mode4(name, canvas, plane, log_size, sprites,
          extra_progs=(), extra_chunks=(), img_base=0x6000):
    # Each image is followed by a 32-bit metadata word per row, which is read
    # only for a sprite whose descriptor has a nonzero metadata byte. Even rows
    # hold a span from texel 2 up to but not including texel size - 2, and a
    # texel in it is drawn only when its alpha bit is set. Odd rows hold a
    # span of the whole row and set bit 31, so the row's texels are drawn
    # without the alpha test.
    size = 1 << log_size
    stride = size * size * 2 + size * 4
    # The images start at $6000 because these fixtures place mode 3 bitmaps
    # of up to 15,000 bytes at $2000, and the largest ends past $4000.
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
           extra_progs=(), extra_chunks=(), img_base=0x6000):
    # A transform is the six signed 8.8 fixed-point matrix words in the
    # order a00, a01, b0, a10, a11, b1.
    size = 1 << log_size
    stride = size * size * 2
    # The images start at $6000 because one of these fixtures places a
    # 12,000-byte mode 3 bitmap at $2000, which ends past $4000.
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
    # Bit 5 of a color is the alpha bit and makes the color opaque when set,
    # so the plane 2 cells have opaque foregrounds and transparent
    # backgrounds.
    cfg3 = bytearray((0, 0)) + le16(10, 20, 200, 100, 0x0800, 0xFFFF)
    bm = bytes((i * 13 + 7) & 0xFF for i in range(200 * 100))
    cfg2 = bytearray((0, 0)) + le16(60, 50, 20, 8, 0x6000, 0xFFFF, 0x6100)
    tmap = bytes((i * 3 + 1) % 4 for i in range(20 * 8))
    tiles = bytes((t * 37 + 5) & 0xFF for t in range(4 * 8))
    cfg1 = bytearray((0, 0)) + le16(80, 60, 10, 3, 0x6800, 0xFFFF, 0xFFFF)
    cells = bytearray()
    for i in range(30):
        cells.append(ord("A") + i % 60)
        cells.append(i & 0xFF)  # VGA ignores the attributes byte.
        cells += le16(0x0020 | (i * 3141), (i * 2718 + 9) & 0xFFDF)
    rom(name, 1,
        [(3, 3, 0x0100, 0, 0, 0), (2, 0, 0x0110, 1, 0, 0),
         (1, 12, 0x0120, 2, 0, 0)],
        [(0x0100, cfg3), (0x0110, cfg2), (0x0120, cfg1),
         (0x0800, bm), (0x6000, tmap), (0x6100, tiles), (0x6800, cells)])


def bands(name):
    cfg2 = bytearray((0, 0)) + le16(-5, 0, 20, 14, 0x0600, 0xFFFF, 0x4000)
    cfg3 = bytearray((0, 0)) + le16(60, 60, 160, 72, 0x8000, 0xFFFF)
    cfg1 = bytearray((0, 0)) + le16(40, 100, 24, 10, 0x0800, 0xFFFF, 0xFFFF)
    cfg4 = bytearray((0, 0)) + le16(170, 136, 8, 5, 0x0B00, 0xFFFF, 0x5000)
    cells = bytearray()
    for i in range(24 * 10):
        cells.extend((ord("A") + i % 60, (i * 5 + 1) & 0xFF,
                      (i * 11 + 2) & 0xFF))
    rom(name, 1,
        [(2, 8, 0x0100, 0, 0, 224), (3, 3, 0x0110, 0, 64, 128),
         (1, 3, 0x0120, 1, 112, 176), (2, 10, 0x0130, 2, 144, 208)],
        [(0x0100, cfg2), (0x0110, cfg3), (0x0120, cfg1), (0x0130, cfg4),
         (0x0600, mode2_map(20, 14)), (0x0800, cells),
         (0x0B00, mode2_map(8, 5)),
         (0x4000, mode2_tiles(8)), (0x5000, mode2_tiles(10)),
         (0x8000, bytes((i * 13 + 7) & 0xFF for i in range(160 * 72)))])


mode3("mode3_8bpp", 3, 3, 8, 64, 64, 10, 20, True)
mode3("mode3_1bpp", 1, 0, 1, 64, 48, 5, 7, False)
mode3("mode3_4bppr", 2, 10, 4, 40, 30, 0, 0, False)
mode3("mode3_16bpp", 4, 4, 16, 32, 16, 100, 50, False)
mode3("mode3_2bpp", 1, 1, 2, 80, 60, 3, 5, True,
      config_ptr=0x0102, pal_ptr=0x0202)
mode3("mode3_4bpp", 3, 2, 4, 100, 80, 17, 9, False)
mode3("mode3_1bppr", 2, 8, 1, 64, 40, 7, 3, False)
# In the RTL, a halfword-aligned config takes five 32-bit fetches, so a config
# pointer of 0xFFF2 puts the fifth fetch past the top of XRAM. That fetch wraps
# to word zero, and the halfword it supplies is beyond the fields mode 3 reads.
mode3("mode3_2bppr", 4, 9, 2, 90, 50, 30, 200, False, config_ptr=0xFFF2)
mode3("mode3_wrap", 1, 3, 8, 50, 40, -37, -23, True,
      x_wrap=True, y_wrap=True, config_ptr=0xFDE2, pal_ptr=0xFE00)

mode1("mode1_1bpp8x8", 3, 0, 30, 12, 4, 6, False, False)
mode1("mode1_4bpp8x16", 1, 10, 20, 8, 8, 5, True, False)
mode1("mode1_4bppr8x8", 2, 1, 24, 10, 0, 0, False, False)
mode1("mode1_8bpp8x8", 1, 3, 16, 9, 3, 2, True, True, config_ptr=0x0102)
mode1("mode1_16bpp8x16", 4, 12, 12, 6, 40, 30, False, False)
mode1("mode1_wrap", 1, 0, 20, 6, -13, -9, True,
      False, x_wrap=True, y_wrap=True, pal_ptr=0x0202)

mode2("mode2_1bpp8", 3, 0x000, 60, 40, 80, 60, False, False, False)
mode2("mode2_2bpp16", 1, 0x009, 12, 8, -15, -10, False, False, True)
mode2("mode2_4bpp8trim", 1, 0x232, 20, 10, -7, -9, False, False, True)
mode2("mode2_8bpp16wrap", 2, 0x00B, 12, 8, 60, 20, True, True, True)
mode2("mode2_16trim", 1, 0x359, 10, 6, 4, 2, False, False, True,
      pal_ptr=0x0206)
mode2("mode2_trimx", 3, 0x022, 30, 12, 12, 20, False, False, True)
mode2("mode2_trimx8", 2, 0x013, 20, 8, 100, 50, False, False, True)
mode2("mode2_trimy", 2, 0x500, 24, 14, 6, 1, False, False, False)
# A tile pointer high enough that the last tiles are addressed past the end of
# XRAM. Those tiles are not drawn, in the renderer and in mode2.sv alike, and
# the renderer never reads past the array.
mode2("mode2_tileoob", 1, 0x002, 20, 10, 5, 5, False, False, True,
      tile_ptr=0xFF80)
composite("mode2_composite")
bands("prog_bands")

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
mode4a("mode4a_id", 1, 0, 4, [
    ((0x100, 0, 0, 0, 0x100, 0), 40, 50, 0),
])
# A pair for test_modes' affine_identity_matches_plain. The identity matrix
# maps the sprite onto the image 1:1, so the affine blit has to land every
# texel where the plain blit does. Both generators build image 0 from the
# same formula, and the plain sprite reads no metadata.
mode4a("mode4a_same", 1, 0, 4, [
    ((0x100, 0, 0, 0, 0x100, 0), 40, 50, 0),
])
mode4("mode4_same", 1, 0, 4, [
    (40, 50, 0, False),
])
mode4a("mode4a_rot", 1, 0, 5, [
    ((0x0DD, 0x080, 0x300, -0x080 & 0xFFFF, 0x0DD, 0x200), 60, 60, 0),
    ((0x080, 0, 0, 0, 0x080, 0), 150, 80, 1),
    ((0x200, 0, 0x300, 0, 0x200, 0x300), 240, 100, 0),
])
def stress(name):
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
mode3("mode3_16bpp_odd", 4, 4, 16, 32, 16, 100, 50, False, data_ptr=0x0801)
mode4("mode4_odd", 1, 0, 3, [
    (20, 30, 0, False), (120, 60, 1, True), (-4, 150, 0, True),
], img_base=0x6001)
mode4a("mode4a_odd", 1, 0, 4, [
    ((0x0DD, 0x080, 0x300, -0x080 & 0xFFFF, 0x0DD, 0x200), 60, 60, 0),
    ((0x100, 0, 0, 0, 0x100, 0), 180, 90, 1),
], img_base=0x6001)

stress("sprite_stress")

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
d += le16(0x0DD, 0x080, 0, -0x080 & 0xFFFF, 0x0DD, 0x2000) \
    + le16(170, 100, 0x8000) + bytes((7, 0))
rom("mode4a_sizes", 1, [(4, 1, 0x0102, 2, 0, 0, 0)],
    [(0x0102, d),
     (0x1000, le16(*(((t * 13 + 5) & 0xFFFF) for t in range(64)))),
     (0x8000, le16(*(((t * 5 + 11) & 0xFFFF) for t in range(16384))))])

mode5("sprite_overrun", 1, 27, 0,
      [(i * 3, 40, 0, 0) for i in range(100)], desc_ptr=0x0800)

# One sprite alone on its row, and a long list with one sprite on the
# row, for what the rest of the list costs.
mode5("mode5_onrow", 1, 9, 0, [(30, 40, 0, 0)])
mode5("mode5_onrow2", 1, 9, 0, [(30, 40, 0, 0), (60, 40, 0, 0)])
mode5("mode5_on32", 1, 17, 0, [(30, 40, 0, 0)])
mode5("mode5_on32x2", 1, 17, 0, [(30, 40, 0, 0), (80, 40, 0, 0)])
mode4("mode4_onrow", 1, 0, 4, [(30, 40, 0, False)])
mode4("mode4_onrow2", 1, 0, 4, [(30, 40, 0, False), (60, 40, 0, False)])
mode4("mode4_on8", 1, 0, 3, [(30, 40, 0, False)])
mode4("mode4_on8x2", 1, 0, 3, [(30, 40, 0, False), (60, 40, 0, False)])
mode4a("mode4a_onrow2", 1, 0, 4, [
    ((0x100, 0, 0, 0, 0x100, 0), 30, 40, 0),
    ((0x100, 0, 0, 0, 0x100, 0), 60, 40, 0),
])
mode4a("mode4a_offrow", 1, 0, 4,
       [((0x100, 0, 0, 0, 0x100, 0), 30, 40, 0)]
       + [((0x100, 0, 0, 0, 0x100, 0), i * 3, -100, 0) for i in range(200)])
mode5("mode5_offrow", 1, 9, 0,
      [(30, 40, 0, 0)] + [(i * 3, -100, 0, 0) for i in range(200)],
      desc_ptr=0x0800)
mode4("mode4_offrow", 1, 0, 4,
      [(30, 40, 0, False)] + [(i * 3, -100, 0, False) for i in range(200)])

# The same stack cut to sixty. A 64-pixel 8bpp sprite is 37 clocks once its
# palette is cached, and these images cycle through all 128 words of it,
# 384 clocks once a row: sixty need more than one line's 1,600 clocks and
# fewer than the two a 320 wide row has, and the hundred above need more
# than two. Between them they bracket the row's budget.
mode5("sprite_pair", 1, 27, 0,
      [(i * 5, 40, 0, 0) for i in range(60)])

# Custom mode 5 sprites. The 1 bpp widths 12, 20 and 4 leave bits unread at
# the end of a row.
mode5c("mode5c_depths", 1, 0,
       [(12, 8, 1), (20, 4, 1), (4, 16, 1), (16, 24, 2), (40, 16, 4),
        (8, 48, 8), (24, 12, 8)],
       [(10, 10, 0, 0, 0), (40, 10, 1, 0, 0), (80, 10, 2, 1, 0),
        (100, 10, 3, 0, 0), (140, 10, 4, "odd", 0), (200, 10, 5, None, 0),
        (230, 10, 6, 0, 0),
        (10, 100, 0, 1, 0), (40, 100, 1, None, 0), (100, 100, 3, 1, 0),
        (140, 100, 4, 0, 0), (230, 100, 6, 1, 0)],
       n_pals=2)

for flag, opt in (("hflip", MODE5_HFLIP), ("vflip", MODE5_VFLIP),
                  ("hdbl", MODE5_HDBL), ("vdbl", MODE5_VDBL),
                  ("all4", MODE5_ALL4)):
    mode5c_pair(f"mode5c_{flag}", 1, 0,
                [(24, 16, 4), (16, 24, 8), (12, 8, 1)],
                [(20, 20, 0, 0, opt), (80, 20, 1, None, opt),
                 (140, 20, 2, 0, opt)])

mode5c_pair("mode5c_clip", 1, 0, [(8, 8, 4), (12, 8, 1)], [
    (-5, 20, 0, 0, 0), (316, 20, 0, 0, 0),
    (30, -3, 0, 0, 0), (30, 236, 0, 0, 0),
    (-7, 60, 0, 0, MODE5_HFLIP), (316, 60, 0, 0, MODE5_HFLIP),
    (60, -6, 0, 0, MODE5_VFLIP), (60, 236, 0, 0, MODE5_VFLIP),
    (-1, 100, 0, 0, MODE5_HDBL), (305, 100, 0, 0, MODE5_HDBL),
    (-3, 120, 0, 0, MODE5_HDBL),
    (140, -1, 0, 0, MODE5_VDBL), (140, 225, 0, 0, MODE5_VDBL),
    (-1, 150, 0, 0, MODE5_ALL4), (305, 225, 0, 0, MODE5_ALL4),
    (-9, 180, 1, 0, MODE5_HDBL | MODE5_HFLIP),
])
mode5c_pair("mode5c_clip640", 3, 0, [(8, 8, 4)], [
    (636, 24, 0, 0, MODE5_HDBL), (633, 40, 0, 0, MODE5_HDBL),
    (625, 56, 0, 0, MODE5_HDBL),
    (636, 72, 0, 0, MODE5_HDBL | MODE5_HFLIP),
    (633, 88, 0, 0, MODE5_HDBL | MODE5_HFLIP),
    (625, 104, 0, 0, MODE5_HDBL | MODE5_HFLIP),
], extra_progs=[(3, 3, 0x01A0, 0, 0, 0)],
    extra_chunks=[
        (0x01A0, bytearray((0, 0)) + le16(560, 20, 80, 100, 0x2000, 0xFFFF)),
        (0x2000, bytes((i * 13 + 7) & 0xFF for i in range(80 * 100))),
])

MODE5C_HALF = [
    (10, 10, 0, 0, MODE5_HFLIP), (40, 10, 1, 0, MODE5_VFLIP),
    (80, 10, 2, None, MODE5_HDBL), (140, 10, 3, 0, MODE5_VDBL),
    (180, 10, 2, "odd", MODE5_HFLIP | MODE5_VFLIP),
    (220, 10, 0, 0, MODE5_HDBL | MODE5_VDBL),
]
mode5c("mode5c_half", 1, 0, [(12, 8, 1), (16, 24, 2), (24, 16, 4),
                             (16, 16, 8)], MODE5C_HALF, desc_ptr=0x0102)
mode5c("mode5c_half_even", 1, 0, [(12, 8, 1), (16, 24, 2), (24, 16, 4),
                                  (16, 16, 8)], MODE5C_HALF)

# Palette pointers at the limit for each depth: $FE00 is the last in range
# for 8 bpp, $FFE0 for 4 bpp and $FFFC for 1 bpp. The 8 bpp palette at $FE00
# runs to $FFFF, so the 4 bpp palette at $FFC0 and the image at $FFF0 sit
# inside it and are what its words there read as. The image at $FFF2 is one
# byte past the end of XRAM and draws nothing.
pal8 = mode5c_palette(3)
pal4 = le16(0, *((0x0020 | ((i * 2657 + 4 * 97) & 0xFFFF))
                 for i in range(1, 16)))
mode5c("mode5c_pal", 1, 0,
       [(8, 8, 8), (8, 8, 4), (4, 4, 1), (4, 4, 8, 0xFFF0), (4, 4, 8, 0xFFF2)],
       [(10, 10, 0, ("at", 0xFE00), 0), (30, 10, 0, ("at", 0xFE02), 0),
        (50, 10, 0, ("at", 0xFFE0), 0),
        (70, 10, 1, ("at", 0xFFC0), 0), (90, 10, 1, ("at", 0xFFE2), 0),
        (110, 10, 2, None, 0), (120, 10, 2, ("at", 0xFFFC), 0),
        (130, 10, 2, ("at", 0xFFFE), 0),
        (150, 10, 3, 0, 0), (160, 10, 4, 0, 0)],
       extra_chunks=[(0xFE00, pal8[:0x1C0]), (0xFFC0, pal4),
                     (0xFFE0, pal8[0x1E0:0x1F0])])

mode5c("mode5c_size", 1, 0,
       [(4, 4, 1), (4, 4, 8), (8, 8, 4), (64, 64, 1), (64, 64, 8),
        (32, 32, 4), (16, 64, 2)],
       [(10, 10, 0, 0, 0), (20, 10, 1, 0, 0), (30, 10, 2, 0, 0),
        (10, 30, 3, 0, 0), (90, 30, 4, 0, 0),
        (170, 30, 5, 0, MODE5_HDBL | MODE5_VDBL), (250, 30, 6, 0, MODE5_VDBL)])

# The fixed and custom forms of one scene: the images are the same bytes at
# the same addresses, and the custom sprites carry no flags.
mode5("mode5_same", 1, 10, 0, [(40, 50, 0, 0), (-6, 90, 1, 0),
                               (310, 120, 0, None)])
mode5c("mode5c_same", 1, 0, [(16, 16, 4), (16, 16, 4)],
       [(40, 50, 0, 0, 0), (-6, 90, 1, 0, 0), (310, 120, 0, None, 0)])

# A fixed list on plane 0 and a custom list on plane 1 over the same rows.
mode5c("mode5c_planes", 1, 1, [(32, 32, 4), (16, 16, 8)],
       [(100, 50, 0, 0, MODE5_HFLIP),
        (150, 60, 1, 0, MODE5_HDBL | MODE5_VDBL)],
       extra_progs=[(5, 27, 0x0200, 2, 0, 0, 0)],
       extra_chunks=[(0x0200, le16(10, 40, 0x8000, 0x0400)
                      + le16(220, 40, 0x8000, 0x0400)),
                     (0x8000, mode5c_image(2, 64, 64, 8))])


def mode5c_full(name, desc_ptr):
    sprites = []
    for row, y in enumerate((40, 60, 80)):
        sprites.append((10, y, 0, None if row == 1 else 0, 0))
        sprites += [(x, y, 1, 0, 0) for x in range(40, 140, 10)]
    mode5c(name, 1, 0, [(24, 8, 2), (8, 8, 1)], sprites, desc_ptr=desc_ptr)


mode5c_full("mode5c_full", 0x0100)
mode5c_full("mode5c_full2", 0x0102)

mode5c("mode5c_on32", 1, 0, [(32, 32, 4)], [(30, 40, 0, 0, 0)])
mode5c("mode5c_hdbl_on32", 1, 0, [(16, 32, 4)], [(30, 40, 0, 0, MODE5_HDBL)])
mode5c("mode5c_hflip_on32", 1, 0, [(32, 32, 4)],
       [(30, 40, 0, 0, MODE5_HFLIP)])
mode5c("mode5c_onrow2", 1, 0, [(32, 32, 4)],
       [(30, 40, 0, 0, 0), (80, 40, 0, 0, 0)])
mode5c("mode5c_onrow", 1, 0, [(16, 16, 4)], [(30, 40, 0, 0, 0)],
       desc_ptr=0x0800)
mode5c("mode5c_offrow", 1, 0, [(16, 16, 4)],
       [(30, 40, 0, 0, 0)] + [(i * 3, -100, 0, 0, 0) for i in range(200)],
       desc_ptr=0x0800)
mode5c("mode5c_stack", 1, 0, [(32, 16, 8)],
       [(i * 12, 40, 0, 0, MODE5_HDBL if i & 1 else MODE5_HFLIP)
        for i in range(24)])
# The 32x8 8 bpp image holds every palette index once, so each sprite walks
# the whole palette.
mode5c("mode5c_pair", 1, 0, [(32, 8, 8)],
       [(i * 7, 40, 0, 0, MODE5_HDBL) for i in range(40)], desc_ptr=0x0800)
mode5c("mode5c_overrun", 1, 0, [(32, 8, 8)],
       [(i * 3, 40, 0, 0, MODE5_HDBL | MODE5_HFLIP) for i in range(100)],
       desc_ptr=0x0800)

# The text hides the cursor because a blinking cursor would make the frame
# a suite compares depend on when it is captured.
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

p = prog(4, [(3, 3, 0x0100, 0, 0, 0)], speak=MODE0_SAY, stop=False)
p.xreg(1, 0, 0, 0)
p.stp()
r = Rom().program(p)
r.record(0x10000 + 0x0100,
         bytearray((0, 0)) + le16(20, 10, 200, 150, 0x0800, 0xFFFF))
r.record(0x10000 + 0x0800,
         bytes((i * 13 + 7) & 0xFF for i in range(200 * 150)))
# The manifest records canvas 0 because the program ends on the console
# canvas, and a suite captures the canvas the program ends on.
note("mode0_return", 0)
r.write(OUT / "mode0_return.rp6502")

cfg0 = bytearray((0, 0)) + le16(10, 20, 64, 64, 0x0800, 0x0200)
cfg1 = bytearray((0, 0)) + le16(40, 30, 64, 64, 0x1800, 0x0600)
rom("fill_heavy640", 3,
    [(3, 3, 0x0100, 0, 0, 0), (3, 3, 0x0140, 1, 0, 0)],
    [(0x0100, cfg0), (0x0140, cfg1),
     (0x0800, bytes((i * 13 + 7) & 0xFF for i in range(64 * 64))),
     (0x1800, bytes((i * 11 + 3) & 0xFF for i in range(64 * 64))),
     (0x0200, le16(*((0x0020 | (i * 2657)) for i in range(256)))),
     (0x0600, le16(*((0x0020 | (i * 1031 + 5)) for i in range(256))))])

# Three full-width 8bpp fills with XRAM palettes, the heaviest serial fill a
# 640-wide line can ask for: 128 palette words and 640 pixels apiece.
cfg_a = bytearray((0, 0)) + le16(0, 0, 640, 16, 0x1000, 0x0200)
cfg_b = bytearray((0, 0)) + le16(0, 0, 640, 16, 0x3800, 0x0400)
cfg_c = bytearray((0, 0)) + le16(0, 0, 640, 16, 0x6000, 0x0600)
rom("fill_three640", 3,
    [(3, 3, 0x0100, 0, 0, 0), (3, 3, 0x0140, 1, 0, 0),
     (3, 3, 0x0180, 2, 0, 0)],
    [(0x0100, cfg_a), (0x0140, cfg_b), (0x0180, cfg_c),
     (0x1000, bytes((i * 13 + 7) & 0xFF for i in range(640 * 16))),
     (0x3800, bytes((i * 11 + 3) & 0xFF for i in range(640 * 16))),
     (0x6000, bytes((i * 7 + 1) & 0xFF for i in range(640 * 16))),
     (0x0200, le16(*((0x0020 | (i * 2657)) for i in range(256)))),
     (0x0400, le16(*(((i & 1) << 5 | (i * 1031 + 5)) for i in range(256)))),
     (0x0600, le16(*(((i >> 1 & 1) << 5 | (i * 733 + 9))
                     for i in range(256))))])

# Three 80-column 8bpp text planes with XRAM palettes, every cell of every
# plane on the line: the fill the pico's three-plane test runs.
def text_cells(w, h, k):
    cells = bytearray()
    for i in range(w * h):
        cells.extend((ord("A") + i % 60, (i * 5 + 1 + k) & 0xFF,
                      (i * 11 + 2 + k) & 0xFF))
    return cells
cfg_a = bytearray((0, 0)) + le16(0, 0, 80, 4, 0x1000, 0x0200, 0xFFFF)
cfg_b = bytearray((0, 0)) + le16(0, 0, 80, 4, 0x1400, 0x0400, 0xFFFF)
cfg_c = bytearray((0, 0)) + le16(0, 0, 80, 4, 0x1800, 0x0600, 0xFFFF)
rom("text_three640", 3,
    [(1, 3, 0x0100, 0, 0, 0), (1, 3, 0x0140, 1, 0, 0),
     (1, 3, 0x0180, 2, 0, 0)],
    [(0x0100, cfg_a), (0x0140, cfg_b), (0x0180, cfg_c),
     (0x1000, text_cells(80, 4, 0)), (0x1400, text_cells(80, 4, 7)),
     (0x1800, text_cells(80, 4, 13)),
     (0x0200, le16(*((0x0020 | (i * 2657 + 5)) for i in range(256)))),
     (0x0400, le16(*(((i & 1) << 5 | (i * 1031 + 5)) for i in range(256)))),
     (0x0600, le16(*(((i >> 1 & 1) << 5 | (i * 733 + 9))
                     for i in range(256))))])

# Sixteen-bit pixels at an odd byte, so every other pixel straddles a word,
# and a wrap every 32 pixels, so each segment ends on a straddle and the
# next segment starts right behind it.
mode3("mode3_16odd_wrap", 4, 4, 16, 32, 16, -10, 50, False, x_wrap=True,
      data_ptr=0x0801)

# Two 16bpp bitmaps on one line whose data pointers differ in byte parity. A
# 16bpp bitmap at an odd byte is the only fill whose words start part way into
# a pixel, so this is the only way one plane can leave a bit phase behind for
# the next plane to inherit.
cfg_odd = bytearray((0, 1)) + le16(0, 0, 640, 16, 0x0801, 0xFFFF)
cfg_even = bytearray((0, 1)) + le16(0, 0, 640, 16, 0x8000, 0xFFFF)
rom("mode3_16parity", 3,
    [(3, 4, 0x0100, 0, 0, 0), (3, 4, 0x0180, 1, 0, 0)],
    [(0x0100, cfg_odd), (0x0180, cfg_even),
     (0x0801, bytes((i * 13 + 7) & 0xFF for i in range(640 * 16 * 2))),
     (0x8000, bytes((i * 11 + 3) & 0xFF for i in range(640 * 16 * 2)))])

# Three full-width 16bpp bitmaps on one line, the most expensive fill a 640
# wide canvas can be asked for: every pixel is its own halfword, so a pair of
# them is a whole word and the line leans on XRAM as hard as it can.
cfg16 = [bytearray((0, 1)) + le16(0, 0, 640, 8, base, 0xFFFF)
         for base in (0x1000, 0x4000, 0x7000)]
rom("fill_three640_16bpp", 3,
    [(3, 4, 0x0100, 0, 0, 0), (3, 4, 0x0140, 1, 0, 0),
     (3, 4, 0x0180, 2, 0, 0)],
    [(0x0100, cfg16[0]), (0x0140, cfg16[1]), (0x0180, cfg16[2]),
     (0x1000, bytes((i * 13 + 7) & 0xFF for i in range(640 * 8 * 2))),
     (0x4000, bytes((i * 11 + 3) & 0xFF for i in range(640 * 8 * 2))),
     (0x7000, bytes((i * 7 + 1) & 0xFF for i in range(640 * 8 * 2)))])

# The same three 16bpp planes with a stack of sprites over them. Fill and
# sprites are separate engines, but they read through the same XRAM port, and
# a 16bpp fill is the one that wants a word every clock, so this is where the
# port itself is the limit rather than either engine.
spr = bytearray()
for i in range(16):
    spr += le16(i * 38, 100, 0xA000 + (i % 4) * 576) + bytes((4, 0))
spr_img = []
for im in range(4):
    img = le16(*(((im * 47 + t * 13 + 5) & 0xFFFF) for t in range(16 * 16)))
    meta = bytearray()
    for r in range(16):
        meta += ((1 << 31) | 16 if r & 1
                 else (2 << 16) | 14).to_bytes(4, "little")
    spr_img.append((0xA000 + im * 576, img + meta))
rom("fill_three640_16bpp_spr", 3,
    [(3, 4, 0x0100, 0, 0, 0), (3, 4, 0x0140, 1, 0, 0),
     (3, 4, 0x0180, 2, 0, 0), (4, 0, 0x0200, 16, 2, 0, 0)],
    [(0x0100, cfg16[0]), (0x0140, cfg16[1]), (0x0180, cfg16[2]),
     (0x0200, spr),
     (0x1000, bytes((i * 13 + 7) & 0xFF for i in range(640 * 8 * 2))),
     (0x4000, bytes((i * 11 + 3) & 0xFF for i in range(640 * 8 * 2))),
     (0x7000, bytes((i * 7 + 1) & 0xFF for i in range(640 * 8 * 2)))]
    + spr_img)

if ARGS.emit_manifest:
    ARGS.emit_manifest.write_text(
        "".join(f"{n} {w} {h}\n" for n, w, h in sorted(MANIFEST)))
