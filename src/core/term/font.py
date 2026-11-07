#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The fonts in core/term/font.c: the image the Pocket loads, with its layout
# for the Pocket firmware, the tables font_init builds for the tests, and
# with --png a code page chart of each font.

import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import gen

FONT_C = "src/core/term/font.c"

DEC_MAP_BLANK = 0xFFFF
DEC_MAP_ASCII = 0x10000

OFF_FONT16 = 0x0000
OFF_FONT8 = 0x1000
OFF_ITALIC16 = 0x1800
OFF_DEC16 = 0x2000
OFF_DEC8 = 0x2200
OFF_PAGES = 0x2400

PAGE_16 = 16 * 128
PAGE_8 = 8 * 128
PAGE_STRIDE = PAGE_16 + PAGE_8


def parse_array(text, name):
    m = re.search(re.escape(name) + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        sys.exit(f"font.py: array {name} not found")
    return [int(t, 0) for t in re.findall(r"0[xX][0-9a-fA-F]+", m.group(1))]


def parse_dec_map(text):
    m = re.search(r"dec_glyph_map\[0x20\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        sys.exit("font.py: dec_glyph_map not found")
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
            else:
                v |= int(tok.rstrip("u"), 0)
        entries.append(v)
    if len(entries) != 0x20:
        sys.exit(f"font.py: dec_glyph_map has {len(entries)} entries")
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
    dec_map = parse_dec_map(text)

    # font16 and font8 have a 256-byte stride and blank high halves, which
    # matches the tables font_init in core/term/font.c has built just before
    # it calls font_set_code_page(437).
    font16 = [0] * 4096
    for row in range(16):
        for code in range(128):
            font16[row * 256 + code] = ascii16[row * 128 + code]
    font8 = [0] * 2048
    for row in range(8):
        for code in range(128):
            font8[row * 256 + code] = ascii8[row * 128 + code]

    # font.c builds the DEC graphics from the CP437 glyphs whatever code
    # page is loaded, so this does the same.
    dec16 = [0] * 512
    for row in range(16):
        for idx in range(0x20):
            m = dec_map[idx]
            if m == DEC_MAP_BLANK:
                continue
            if m & DEC_MAP_ASCII:
                dec16[row * 32 + idx] = ascii16[row * 128 + (m & 0xFF)]
            else:
                dec16[row * 32 + idx] = cp437_16[row * 128 + ((m & 0xFF) - 0x80)]

    dec8 = [0] * 256
    for row in range(8):
        for idx in range(0x20):
            m = dec_map[idx]
            if m == DEC_MAP_BLANK:
                continue
            if m & DEC_MAP_ASCII:
                dec8[row * 32 + idx] = ascii8[row * 128 + (m & 0xFF)]
            else:
                dec8[row * 32 + idx] = cp437_8[row * 128 + ((m & 0xFF) - 0x80)]

    italic16 = [0] * 2048
    for row in range(16):
        for code in range(128):
            italic16[row * 128 + code] = italic_src[row * 128 + code]

    pages = code_pages(text)
    highs = []
    for cp in pages:
        highs.append((parse_array(text, f"FONT16_CP{cp}"),
                      parse_array(text, f"FONT8_CP{cp}")))
    return font16, dec16, dec8, italic16, font8, pages, highs


def build_asset(font16, dec16, dec8, italic16, font8, highs):
    img = bytearray(OFF_PAGES + PAGE_STRIDE * len(highs))
    img[OFF_FONT16:OFF_FONT16 + 4096] = bytes(font16)
    img[OFF_FONT8:OFF_FONT8 + 2048] = bytes(font8)
    img[OFF_ITALIC16:OFF_ITALIC16 + 2048] = bytes(italic16)
    img[OFF_DEC16:OFF_DEC16 + 512] = bytes(dec16)
    img[OFF_DEC8:OFF_DEC8 + 256] = bytes(dec8)
    for i, (hi16, hi8) in enumerate(highs):
        at = OFF_PAGES + PAGE_STRIDE * i
        img[at:at + PAGE_16] = bytes(hi16)
        img[at + PAGE_16:at + PAGE_STRIDE] = bytes(hi8)
    return bytes(img)


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


def emit_rom_h(path, pages):
    out = [HEADER,
           "#ifndef _FONT_ROM_H_", "#define _FONT_ROM_H_", "",
           "#include <stdint.h>", "",
           "/* Offsets into font_rom.bin, and the code pages it carries",
           " * in the order their high halves follow the base image. */",
           f"#define FONT_ROM_OFF_FONT16 0x{OFF_FONT16:04X}",
           f"#define FONT_ROM_OFF_FONT8 0x{OFF_FONT8:04X}",
           f"#define FONT_ROM_OFF_ITALIC16 0x{OFF_ITALIC16:04X}",
           f"#define FONT_ROM_OFF_DEC16 0x{OFF_DEC16:04X}",
           f"#define FONT_ROM_OFF_DEC8 0x{OFF_DEC8:04X}",
           f"#define FONT_ROM_OFF_PAGES 0x{OFF_PAGES:04X}",
           f"#define FONT_ROM_PAGE_16 {PAGE_16}",
           f"#define FONT_ROM_PAGE_8 {PAGE_8}",
           f"#define FONT_ROM_PAGE_STRIDE {PAGE_STRIDE}", "",
           f"#define FONT_ROM_PAGE_COUNT {len(pages)}",
           "static const uint16_t FONT_ROM_PAGES[FONT_ROM_PAGE_COUNT] = {",
           "    " + ", ".join(str(cp) for cp in pages) + ",",
           "};", "",
           "#endif /* _FONT_ROM_H_ */", ""]
    Path(path).write_text("\n".join(out))


def emit_tables_h(path, font16, dec16, dec8, italic16, font8, pages, highs):
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
           c_array("FONT_DEC_16", dec16), "",
           c_array("FONT_DEC_8", dec8), "",
           c_array("ITALIC16", italic16), "",
           c_array("FONT8", f8), "",
           "#endif /* _FONT_TABLES_H_ */", ""]
    Path(path).write_text("\n".join(out))


# The code page charts: each FONT8_* and FONT16_* array in font.c as a
# 16 by 8 grid of its glyphs, labelled in hex, as a greyscale PNG named
# after the array's flash section.

CHART_SCALE = 4
CHART_FG = 255
CHART_BG = 0
CHART_HEADER_BG = 32
CHART_LABEL_FG = 200
HEX_DIGITS = "0123456789ABCDEF"

BLOCK_RE = re.compile(
    r'static\s+const\s+HOST_IN_FLASH\("([^"]+)"\)\s+uint8_t\s+'
    r'(FONT(\d+)_\w+)\s*\[\]\s*=\s*\{([^}]+)\};', re.S)


def parse_blocks(text):
    blocks = []
    for m in BLOCK_RE.finditer(text):
        label, name, height = m.group(1), m.group(2), int(m.group(3))
        data = bytes(int(b, 16) for b in re.findall(r"0x[0-9a-fA-F]+", m.group(4)))
        if len(data) != 128 * height:
            sys.exit(f"font.py: {name} has {len(data)} bytes, want {128 * height}")
        blocks.append((name, label, height, data))
    return blocks


def write_png(path, width, height, pixels):
    def chunk(tag, payload):
        crc = zlib.crc32(tag + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)

    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += pixels[y * width:(y + 1) * width]
    Path(path).write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b""))


def fill_rect(pixels, img_w, x0, y0, w, h, color):
    run = bytes([color]) * w
    for y in range(y0, y0 + h):
        pixels[y * img_w + x0:y * img_w + x0 + w] = run


def blit_glyph(pixels, img_w, x0, y0, rows, fg, bg):
    fg_run = bytes([fg]) * CHART_SCALE
    bg_run = bytes([bg]) * CHART_SCALE
    for r, b in enumerate(rows):
        line = bytearray()
        for p in range(8):
            line += fg_run if b & (0x80 >> p) else bg_run
        for dy in range(CHART_SCALE):
            at = (y0 + r * CHART_SCALE + dy) * img_w + x0
            pixels[at:at + len(line)] = line


def glyph(data, height, code):
    return bytes(data[r * 128 + code] for r in range(height))


def render_chart(path, labels, base, height, data):
    cell_w = 8 * CHART_SCALE
    cell_h = height * CHART_SCALE
    img_w = cell_w * 17
    img_h = cell_h * 9
    pixels = bytearray([CHART_BG] * (img_w * img_h))
    fill_rect(pixels, img_w, 0, 0, img_w, cell_h, CHART_HEADER_BG)
    fill_rect(pixels, img_w, 0, 0, cell_w, img_h, CHART_HEADER_BG)
    label_y = (cell_h - 8 * CHART_SCALE) // 2
    for col in range(16):
        blit_glyph(pixels, img_w, (1 + col) * cell_w, label_y,
                   labels[HEX_DIGITS[col]], CHART_LABEL_FG, CHART_HEADER_BG)
    for row in range(8):
        blit_glyph(pixels, img_w, 0, (1 + row) * cell_h + label_y,
                   labels[HEX_DIGITS[(base >> 4) + row]],
                   CHART_LABEL_FG, CHART_HEADER_BG)
    for code in range(128):
        blit_glyph(pixels, img_w, (1 + (code & 15)) * cell_w,
                   (1 + (code >> 4)) * cell_h, glyph(data, height, code),
                   CHART_FG, CHART_BG)
    write_png(path, img_w, img_h, pixels)


def emit_charts(out, text):
    blocks = parse_blocks(text)
    label_data = next((d for n, _, _, d in blocks if n == "FONT8_ASCII"), None)
    if label_data is None:
        sys.exit("font.py: FONT8_ASCII not found")
    labels = {ch: glyph(label_data, 8, ord(ch)) for ch in HEX_DIGITS}
    Path(out).mkdir(parents=True, exist_ok=True)
    for name, label, height, data in blocks:
        base = 0 if name.endswith("_ASCII") else 128
        render_chart(Path(out) / f"{label}.png", labels, base, height, data)


def main():
    text = (gen.ROOT / FONT_C).read_text(encoding="utf-8")
    font16, dec16, dec8, italic16, font8, pages, highs = build_tables(text)
    a = gen.main(
        inputs=[FONT_C],
        c={"font_rom.h": lambda p: emit_rom_h(p, pages),
           "font_tables.h": lambda p: emit_tables_h(
               p, font16, dec16, dec8, italic16, font8, pages, highs)},
        bin={"font_rom.bin": lambda p: Path(p).write_bytes(
            build_asset(font16, dec16, dec8, italic16, font8, highs))},
        extra=lambda ap: ap.add_argument("--png", metavar="DIR"))
    if a.png:
        emit_charts(a.png, text)


if __name__ == "__main__":
    main()
