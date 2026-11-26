#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The fonts in core/term/font.c: the tables font_init builds, for the tests,
# and with --png the code charts of the manual's Fonts page.

import html
import re
import sys
import unicodedata
from pathlib import Path

import build

FONT_C = "src/core/term/font.c"

DEC_MAP_BLANK = 0xFFFF
DEC_MAP_ASCII = 0x10000
DEC_MAP_DRAWN = 0x20000
TECH_MAP_DRAWN = 0x40000


def parse_array(text, name):
    m = re.search(re.escape(name) + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        sys.exit(f"font.py: array {name} not found")
    return [int(t, 0) for t in re.findall(r"0[xX][0-9a-fA-F]+", m.group(1))]


def parse_map(text, name, count):
    m = re.search(re.escape(name) + r"\[0x[0-9A-Fa-f]+\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        sys.exit(f"font.py: {name} not found")
    entries = []
    for line in m.group(1).splitlines():
        line = re.sub(r"/\*.*?\*/", "", line)
        line = re.sub(r"//.*", "", line)
        line = line.strip().rstrip(",")
        if not line:
            continue
        v = 0
        for tok in line.split("|"):
            tok = tok.strip()
            if tok == "DEC_MAP_BLANK":
                v |= DEC_MAP_BLANK
            elif tok == "DEC_MAP_ASCII":
                v |= DEC_MAP_ASCII
            elif tok == "DEC_MAP_DRAWN":
                v |= DEC_MAP_DRAWN
            elif tok == "TECH_MAP_DRAWN":
                v |= TECH_MAP_DRAWN
            else:
                v |= int(tok.rstrip("u"), 0)
        entries.append(v)
    if len(entries) != count:
        sys.exit(f"font.py: {name} has {len(entries)} entries")
    return entries


def code_pages(text):
    pages = []
    for cp in re.findall(r"FONT16_CP(\d+)\b", text):
        cp = int(cp)
        if cp not in pages:
            pages.append(cp)
    for cp in pages:
        for name, want in ((f"FONT16_CP{cp}", 2048), (f"FONT8_CP{cp}", 1024)):
            if len(parse_array(text, name)) != want:
                sys.exit(f"font.py: {name} is not {want} bytes")
    return pages


def build_tables(text):
    ascii16 = parse_array(text, "FONT16_ASCII")
    cp437_16 = parse_array(text, "FONT16_CP437")
    italic_src = parse_array(text, "FONT16_ASCII_ITALIC")
    ascii8 = parse_array(text, "FONT8_ASCII")
    cp437_8 = parse_array(text, "FONT8_CP437")
    for name, arr, want in (("FONT16_ASCII", ascii16, 2048),
                            ("FONT16_ASCII_ITALIC", italic_src, 2048),
                            ("FONT8_ASCII", ascii8, 1024)):
        if len(arr) != want:
            sys.exit(f"font.py: {name} has {len(arr)} bytes, want {want}")
    dec_map = parse_map(text, "dec_glyph_map", 0x20)
    tech_map = parse_map(text, "tech_glyph_map", 0x5E)
    drawn = {
        DEC_MAP_DRAWN: (parse_array(text, "FONT16_DEC_DRAWN"),
                        parse_array(text, "FONT8_DEC_DRAWN")),
        TECH_MAP_DRAWN: (parse_array(text, "FONT16_TECH_DRAWN"),
                         parse_array(text, "FONT8_TECH_DRAWN")),
    }

    # Each font is two tables of 256 glyphs with a 256-byte stride: the
    # code page's, its high half blank here, and then the italics of ASCII
    # (ASCII itself in the 8-row font) with DEC Technical at 0x80 to 0xDD
    # and DEC Special Graphics at 0xDF to 0xFE, as font_build in
    # core/term/font.c builds them.
    font16 = [0] * 8192
    for row in range(16):
        for code in range(128):
            font16[row * 256 + code] = ascii16[row * 128 + code]
            font16[4096 + row * 256 + code] = italic_src[row * 128 + code]
    font8 = [0] * 4096
    for row in range(8):
        for code in range(128):
            font8[row * 256 + code] = ascii8[row * 128 + code]
            font8[2048 + row * 256 + code] = ascii8[row * 128 + code]

    # font.c builds the DEC sets from the CP437 glyphs whatever code page is
    # loaded, so this does the same.
    def mapped(m, row):
        if m == DEC_MAP_BLANK:
            return 0, 0
        n = m & 0xFF
        for flag, (d16, d8) in drawn.items():
            if m & flag:
                return d16[n * 16 + row], d8[n * 8 + row] if row < 8 else 0
        if m & DEC_MAP_ASCII:
            return ascii16[row * 128 + n], ascii8[row * 128 + n] if row < 8 else 0
        return cp437_16[row * 128 + n - 0x80], cp437_8[row * 128 + n - 0x80] if row < 8 else 0

    for table, base in ((tech_map, 0x80), (dec_map, 0xDF)):
        for idx, m in enumerate(table):
            for row in range(16):
                v16, v8 = mapped(m, row)
                font16[4096 + row * 256 + base + idx] = v16
                if row < 8:
                    font8[2048 + row * 256 + base + idx] = v8

    pages = code_pages(text)
    highs = []
    for cp in pages:
        highs.append((parse_array(text, f"FONT16_CP{cp}"),
                      parse_array(text, f"FONT8_CP{cp}")))
    return font16, font8, pages, highs


def c_array(name, data):
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 16):
        row = ", ".join(f"0x{b:02X}" for b in data[i:i + 16])
        lines.append(f"    {row},")
    lines.append("};")
    return "\n".join(lines)


HEADER = """\
/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Generated by src/core/term/font.py from src/core/term/font.c -- do not edit.
 */
"""


def emit_tables_h(path, font16, font8, pages, highs):
    # font_init ends by applying code page 437, so the tables that
    # tests/rtl/vga/test_font.cpp compares against font_init's carry the
    # 437 image.
    hi16, hi8 = highs[pages.index(437)]
    f16 = list(font16)
    f8 = list(font8)
    for row in range(16):
        f16[row * 256 + 128:row * 256 + 256] = hi16[row * 128:row * 128 + 128]
    for row in range(8):
        f8[row * 256 + 128:row * 256 + 256] = hi8[row * 128:row * 128 + 128]
    out = [HEADER,
           "#ifndef _FONT_TABLES_H_", "#define _FONT_TABLES_H_",
           "", "#include <stdint.h>", "",
           c_array("FONT16", f16), "",
           c_array("FONT8", f8), "",
           "#endif /* _FONT_TABLES_H_ */", ""]
    build.write_text(path, "\n".join(out))


# The code charts of the manual's Fonts page. Each set of 128 glyphs is a
# PNG of the glyphs in a 16 by 8 grid, a glyph's place being its code with
# the top bit clear, each pixel of a glyph one pixel of the PNG, ink opaque
# black and the rest clear. Beside it is an HTML chart for the page to take
# in, a cell for each glyph with its code in hex and decimal and the code
# point the machine copies it as.
# The glyphs are those font_init builds, so they are what the machine draws.

def sprite(path, height, glyph_row):
    """glyph_row(code, row) is the byte of a row of glyph code (0 to 127)
    of the set, or None for a code the set does not have."""
    width = 16 * 8
    pixels = bytearray(width * 8 * height * 2)
    for code in range(128):
        x0, y0 = (code & 15) * 8, (code >> 4) * height
        for row in range(height):
            b = glyph_row(code, row)
            if not b:
                continue
            for p in range(8):
                if b & (0x80 >> p):
                    pixels[((y0 + row) * width + x0 + p) * 2 + 1] = 255
    build.write_png(path, width, 8 * height, pixels, 2)


CELL_C = "src/core/term/cell.c"


def cell_unicode():
    """The code points cell.c copies the C0 glyphs, DEL, DEC Special
    Graphics and DEC Technical as, None where it copies a space for a
    glyph Unicode lacks."""
    text = (build.ROOT / CELL_C).read_text(encoding="utf-8")

    def table(name):
        m = re.search(re.escape(name) + r"\[[^]]*\]\s*=\s*\{(.*?)\};", text, re.S)
        if not m:
            sys.exit(f"font.py: {name} not found in {CELL_C}")
        body = re.sub(r"/\*.*?\*/", "", m.group(1))
        return [None if t.strip() == "' '" else int(t, 0)
                for t in body.split(",") if t.strip()]
    m = re.search(r"#define CELL_DEL_GLYPH (0x[0-9A-Fa-f]+)", text)
    if not m:
        sys.exit(f"font.py: CELL_DEL_GLYPH not found in {CELL_C}")
    return table("cell_c0"), int(m.group(1), 16), table("cell_dec"), table("cell_tech")


def char_name(u):
    if u is None:
        return ""
    try:
        return unicodedata.name(chr(u)).lower()
    except ValueError:
        return f"U+{u:04X}"


def chart_html(path, name, height, glyph_row, first_code, unicode_of):
    """unicode_of(code) is the code point of glyph code, or None. The chart
    is a grid of cells, each the glyph with its code in hex and decimal and
    its character's Unicode code point, and the character's name for a
    tooltip."""
    sprite_url = f"_static/fonts/{name}.png"
    out = [f'<div class="font-chart font-chart-{height}">']
    # A set of fewer than 128 glyphs shows only the rows of 8 it is in.
    have = [c for c in range(128) if glyph_row(c, 0) is not None]
    for code in range(have[0] & ~7, (have[-1] | 7) + 1):
        n = first_code + code
        if glyph_row(code, 0) is None:
            out.append('<div class="font-cell"></div>')
            continue
        u = unicode_of(code)
        title = f"0x{n:02X}: {char_name(u)}" if u is not None else f"0x{n:02X}"
        uni = f"U+{u:04X}" if u is not None else ""
        out.append(
            f'<div class="font-cell" title="{html.escape(title)}">'
            f'<span class="font-glyph" style="background-image: url({sprite_url}); '
            f'--x: {code & 15}; --y: {code >> 4}"></span>'
            f'<span class="font-code">0x{n:02X} <span>{n}</span></span>'
            f'<span class="font-uni">{uni}</span></div>')
    out.append("</div>")
    build.write_text(path, "\n".join(out) + "\n")


def emit_charts(out, only, font16, font8, pages, highs):
    """Writes the charts into out, or with only, just the chart of that
    name, as cp437-16, while its look is worked on. For the manual:
    PYTHONPATH=src/core src/core/term/font.py \\
        --png ../picocomputer.github.io/docs/source/_static/fonts"""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    fonts = {16: font16, 8: font8}
    uni = build.load_module(build.ROOT / "src/core/str/unicode.py", "unicode")
    uni_pages = dict(zip(uni.PAGES, uni.tables()[0]))

    def table(height, other, base=0, first=0, last=0x7F):
        font = fonts[height]
        at = (height * 256 if other else 0) + base

        def row_of(code, row):
            if not first <= code <= last:
                return None
            return font[at + row * 256 + code]
        return row_of

    c0, delete, dec, tech = cell_unicode()

    def ascii_of(code):
        if code < 0x20:
            return c0[code]
        return delete if code == 0x7F else code

    charts = {}
    for h in (16, 8):
        charts[f"ascii-{h}"] = (h, table(h, False), 0x00, ascii_of)
        # DEC Technical's characters 0x21 to 0x7E are at 0x80 to 0xDD of
        # the second table, DEC Special Graphics' 0x5F to 0x7E at 0xDF on.
        charts[f"dec-technical-{h}"] = (
            h, table(h, True, 0x80 - 0x21, 0x21, 0x7E), 0x00,
            lambda c: tech[c - 0x21] if 0x21 <= c <= 0x7E else None)
        charts[f"dec-graphics-{h}"] = (
            h, table(h, True, 0xDF - 0x5F, 0x5F, 0x7E), 0x00,
            lambda c: dec[c - 0x5F] if 0x5F <= c <= 0x7E else None)
        for cp, (hi16, hi8) in zip(pages, highs):
            hi = hi16 if h == 16 else hi8
            u = uni_pages.get(cp)
            charts[f"cp{cp}-{h}"] = (
                h, lambda code, row, hi=hi: hi[row * 128 + code], 0x80,
                lambda code, u=u: u[code] if u and u[code] else None)
    # The 8-row font has no italics.
    charts["italic-16"] = (16, table(16, True), 0x00, ascii_of)
    if only and only not in charts:
        sys.exit(f"font.py: no chart {only}; there are {', '.join(charts)}")
    for name, (h, rows, first, unicode_of) in charts.items():
        if not only or name == only:
            sprite(out / f"{name}.png", h, rows)
            chart_html(out / f"{name}.html", name, h, rows, first, unicode_of)


def main():
    text = (build.ROOT / FONT_C).read_text(encoding="utf-8")
    font16, font8, pages, highs = build_tables(text)
    a = build.generate(
        inputs=[FONT_C],
        c={"font_tables.h": lambda p: emit_tables_h(
            p, font16, font8, pages, highs)},
        extra=lambda ap: (ap.add_argument("--png", metavar="DIR"),
                          ap.add_argument("--only", metavar="NAME")))
    if a.png:
        emit_charts(a.png, a.only, font16, font8, pages, highs)


if __name__ == "__main__":
    main()
