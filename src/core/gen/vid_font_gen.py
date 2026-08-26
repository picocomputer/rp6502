#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Builds the font asset from src/core/term/font.c. The glyphs are the video
# device's memory, not the firmware's: nothing on the soft CPU reads a
# glyph, it only moves them, so font.c's tables never need to be linked
# into a 64 KB code memory that cannot hold seventeen code pages anyway.
# They ship beside the core and reach the store by copy.
#
# The image replicates font_init exactly — ASCII low halves, the DEC
# Special Graphics table built from dec_glyph_map, the italic face
# verbatim — and then carries every code page's high halves after it, in
# the same row-major order font_set_code_page's memcpys use. So the
# firmware's copy is that memcpy with a different destination, and the
# parity test can compare the asset against the tables emu_core builds at
# runtime, where drift cannot hide.

import argparse
import re
import sys
from pathlib import Path

FONT_C = Path(__file__).resolve().parents[3] / "src/core/term/font.c"

DEC_MAP_BLANK = 0xFFFF
DEC_MAP_ASCII = 0x10000

# The store's four faces, in asset order: offset and length.
OFF_FONT16 = 0x0000
OFF_FONT8 = 0x1000
OFF_ITALIC16 = 0x1800
OFF_DEC16 = 0x2000
OFF_DEC8 = 0x2200
OFF_PAGES = 0x2400

# One code page: sixteen 128-byte rows of font16's high half, then eight
# of font8's.
PAGE_16 = 16 * 128
PAGE_8 = 8 * 128
PAGE_STRIDE = PAGE_16 + PAGE_8


def parse_array(text, name):
    m = re.search(re.escape(name) + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        sys.exit(f"vid_font_gen: array {name} not found")
    return [int(t, 0) for t in re.findall(r"0[xX][0-9a-fA-F]+", m.group(1))]


def parse_dec_map(text):
    m = re.search(r"dec_glyph_map\[0x20\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        sys.exit("vid_font_gen: dec_glyph_map not found")
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
        sys.exit(f"vid_font_gen: dec_glyph_map has {len(entries)} entries")
    return entries


def code_pages(text):
    """Every page font_set_code_page accepts, in font.c's own order."""
    pages = []
    for cp in re.findall(r"FONT16_CP(\d+)\b", text):
        cp = int(cp)
        if cp not in pages:
            pages.append(cp)
    for cp in pages:
        for name, want in ((f"FONT16_CP{cp}", 2048), (f"FONT8_CP{cp}", 1024)):
            if len(parse_array(text, name)) != want:
                sys.exit(f"vid_font_gen: {name} is not {want} bytes")
    return pages


def build_tables():
    text = FONT_C.read_text()
    ascii16 = parse_array(text, "FONT16_ASCII")
    cp437_16 = parse_array(text, "FONT16_CP437")
    italic_src = parse_array(text, "FONT16_ASCII_ITALIC")
    ascii8 = parse_array(text, "FONT8_ASCII")
    cp437_8 = parse_array(text, "FONT8_CP437")
    for name, arr, want in (("FONT16_ASCII", ascii16, 2048),
                            ("FONT16_ASCII_ITALIC", italic_src, 2048),
                            ("FONT8_ASCII", ascii8, 1024)):
        if len(arr) != want:
            sys.exit(f"vid_font_gen: {name} has {len(arr)} bytes, want {want}")
    dec_map = parse_dec_map(text)

    # font16 and font8, row-major with a 256-byte stride exactly as font.c
    # keeps its live tables, high halves blank: this is the image at the
    # end of font_init and before its font_set_code_page(437).
    font16 = [0] * 4096
    for row in range(16):
        for code in range(128):
            font16[row * 256 + code] = ascii16[row * 128 + code]
    font8 = [0] * 2048
    for row in range(8):
        for code in range(128):
            font8[row * 256 + code] = ascii8[row * 128 + code]

    # font_dec_16, row-major over the 0x5F..0x7E window, 32-byte stride.
    # It draws from CP437 whatever page is loaded, the way font.c builds
    # it before any code page is applied.
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

    # font_dec_8, the same window over the 8-row faces.
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

    # italic16, row-major, 128-byte stride, low half only.
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
 * Generated by vid_font_gen.py from src/core/term/font.c - do not edit.
 */
"""


def emit_firmware_header(path, pages):
    out = [HEADER,
           "#ifndef _VID_FONT_ASSET_H_", "#define _VID_FONT_ASSET_H_", "",
           "#include <stdint.h>", "",
           "/* Offsets into the font asset, and the code pages it carries",
           " * in the order their high halves follow the base image. */",
           f"#define VID_FONT_OFF_FONT16 0x{OFF_FONT16:04X}",
           f"#define VID_FONT_OFF_FONT8 0x{OFF_FONT8:04X}",
           f"#define VID_FONT_OFF_ITALIC16 0x{OFF_ITALIC16:04X}",
           f"#define VID_FONT_OFF_DEC16 0x{OFF_DEC16:04X}",
           f"#define VID_FONT_OFF_DEC8 0x{OFF_DEC8:04X}",
           f"#define VID_FONT_OFF_PAGES 0x{OFF_PAGES:04X}",
           f"#define VID_FONT_PAGE_16 {PAGE_16}",
           f"#define VID_FONT_PAGE_8 {PAGE_8}",
           f"#define VID_FONT_PAGE_STRIDE {PAGE_STRIDE}", "",
           f"#define VID_FONT_PAGE_COUNT {len(pages)}",
           "static const uint16_t VID_FONT_PAGES[VID_FONT_PAGE_COUNT] = {",
           "    " + ", ".join(str(cp) for cp in pages) + ",",
           "};", "",
           "#endif /* _VID_FONT_ASSET_H_ */", ""]
    Path(path).write_text("\n".join(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit-bin", metavar="FILE")
    ap.add_argument("--emit-h", metavar="FILE")
    ap.add_argument("--emit-asset-h", metavar="FILE")
    args = ap.parse_args()
    font16, dec16, dec8, italic16, font8, pages, highs = build_tables()
    if args.emit_bin:
        Path(args.emit_bin).write_bytes(
            build_asset(font16, dec16, dec8, italic16, font8, highs))
    if args.emit_asset_h:
        emit_firmware_header(args.emit_asset_h, pages)
    if args.emit_h:
        # The 437 image the parity test wants: emu_core's font_init ends
        # with that page applied, so the test compares like for like.
        cp437 = pages.index(437)
        hi16, hi8 = highs[cp437]
        f16 = list(font16)
        f8 = list(font8)
        for row in range(16):
            f16[row * 256 + 128:row * 256 + 256] = hi16[row * 128:row * 128 + 128]
        for row in range(8):
            f8[row * 256 + 128:row * 256 + 256] = hi8[row * 128:row * 128 + 128]
        out = [HEADER,
               "#ifndef _VID_FONT_TABLES_H_", "#define _VID_FONT_TABLES_H_",
               "", "#include <stdint.h>", "",
               c_array("VID_FONT16", f16), "",
               c_array("VID_FONT_DEC16", dec16), "",
               c_array("VID_FONT_DEC8", dec8), "",
               c_array("VID_ITALIC16", italic16), "",
               c_array("VID_FONT8", f8), "",
               "#endif /* _VID_FONT_TABLES_H_ */", ""]
        Path(args.emit_h).write_text("\n".join(out))


if __name__ == "__main__":
    main()
