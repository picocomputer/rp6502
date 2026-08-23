#!/usr/bin/env python3
"""Dump every FONT{8,16}_{NAME} block in font.c to a codepage-chart PNG.

Stdlib only. Output goes to <repo>/build/fonts/.
"""
import os
import re
import struct
import sys
import zlib

SCALE = 4

FG = 255
BG = 0
HEADER_BG = 32
LABEL_FG = 200

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
FONT_C = os.path.join(SCRIPT_DIR, "font.c")
OUT_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "..", "..", "build", "fonts"))

BLOCK_RE = re.compile(
    r'static\s+const\s+__in_flash\("([^"]+)"\)\s+uint8_t\s+'
    r'(FONT(\d+)_\w+)\s*\[\]\s*=\s*\{([^}]+)\};',
    re.DOTALL,
)
BYTE_RE = re.compile(r'0x[0-9a-fA-F]+')

HEX_DIGITS = "0123456789ABCDEF"


def parse_blocks(src):
    blocks = []
    for m in BLOCK_RE.finditer(src):
        flash_label = m.group(1)
        name = m.group(2)
        height = int(m.group(3))
        data = bytes(int(b, 16) for b in BYTE_RE.findall(m.group(4)))
        expected = 128 * height
        if len(data) != expected:
            raise ValueError(
                f"{name}: expected {expected} bytes ({128} glyphs * {height} rows), "
                f"got {len(data)}"
            )
        blocks.append((name, flash_label, height, data))
    return blocks


def write_png(path, width, height, pixels):
    sig = b"\x89PNG\r\n\x1a\n"

    def chunk(tag, payload):
        crc = zlib.crc32(tag + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(pixels[y * width:(y + 1) * width])
    idat = zlib.compress(bytes(raw), 9)
    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


def fill_rect(pixels, img_w, x0, y0, w, h, color):
    run = bytes([color]) * w
    for y in range(y0, y0 + h):
        off = y * img_w + x0
        pixels[off:off + w] = run


def blit_glyph(pixels, img_w, x0, y0, glyph_rows, scale, fg, bg):
    fg_run = bytes([fg]) * scale
    bg_run = bytes([bg]) * scale
    for r, b in enumerate(glyph_rows):
        scanline = bytearray()
        for p in range(8):
            scanline += fg_run if (b & (0x80 >> p)) else bg_run
        oy = y0 + r * scale
        line_w = 8 * scale
        for dy in range(scale):
            off = (oy + dy) * img_w + x0
            pixels[off:off + line_w] = scanline


def extract_glyph(data, height, ch_idx):
    return bytes(data[r * 128 + ch_idx] for r in range(height))


def render_block(out_path, label_glyphs, base, height, data):
    cell_w = 8 * SCALE
    cell_h = height * SCALE
    img_w = cell_w * 17
    img_h = cell_h * 9

    pixels = bytearray([BG] * (img_w * img_h))

    fill_rect(pixels, img_w, 0, 0, img_w, cell_h, HEADER_BG)
    fill_rect(pixels, img_w, 0, 0, cell_w, img_h, HEADER_BG)

    label_y_off = (cell_h - 8 * SCALE) // 2

    for col in range(16):
        x = (1 + col) * cell_w
        blit_glyph(pixels, img_w, x, label_y_off,
                   label_glyphs[HEX_DIGITS[col]], SCALE, LABEL_FG, HEADER_BG)

    for row in range(8):
        nibble = (base >> 4) + row
        y = (1 + row) * cell_h + label_y_off
        blit_glyph(pixels, img_w, 0, y,
                   label_glyphs[HEX_DIGITS[nibble]], SCALE, LABEL_FG, HEADER_BG)

    for c in range(128):
        col = c & 0x0F
        row = c >> 4
        x = (1 + col) * cell_w
        y = (1 + row) * cell_h
        blit_glyph(pixels, img_w, x, y,
                   extract_glyph(data, height, c), SCALE, FG, BG)

    write_png(out_path, img_w, img_h, pixels)


def main():
    with open(FONT_C) as f:
        src = f.read()
    blocks = parse_blocks(src)

    label_data = next((d for n, _, h, d in blocks if n == "FONT8_ASCII"), None)
    if label_data is None:
        sys.exit("FONT8_ASCII not found in font.c")
    label_glyphs = {ch: extract_glyph(label_data, 8, ord(ch)) for ch in HEX_DIGITS}

    os.makedirs(OUT_DIR, exist_ok=True)
    for name, flash_label, height, data in blocks:
        base = 0 if name.endswith("_ASCII") else 128
        out_path = os.path.join(OUT_DIR, f"{flash_label}.png")
        render_block(out_path, label_glyphs, base, height, data)
        print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
