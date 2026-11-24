#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The pointer shapes the system draws for the tablet, made from their
# outlines here: mode6_shapes.h for core/vga/mode/mode6.c, each size of a
# shape a mode 5 custom sprite of 4 bits a pixel, and with --png a
# picture of every shape in both sizes, for a person to look at.
#
# A shape is white with a black outline, as a modern pointer is. Each one is
# drawn from polygons, circles and lines at 640-canvas size, and at half that
# for the 320-wide canvases, with the outline a pixel wide in both. Each pixel
# is sampled 4 by 4: one that is more than half covered by the shape and its
# outline is drawn, in one of 15 greys from the share of it that is white;
# one that is not is left clear. So the inner edges are smooth, and the outer
# edge is hard, as a pixel has no alpha but on or off.

import math
import sys

import build

SAMPLES = 4      # a side of a pixel's grid of samples
OUTLINE = 1.0    # the black outline's width, in pixels
GREYS = 15       # the grey levels, palette entries 1 to 15; 0 is clear


# ---- the geometry ------------------------------------------------------

def rect(x0, y0, x1, y1):
    return ("poly", [(x0, y0), (x1, y0), (x1, y1), (x0, y1)])


def poly(*pts):
    return ("poly", list(pts))


def circle(cx, cy, r, n=48):
    return ("poly", [(cx + r * math.cos(2 * math.pi * i / n),
                      cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)])


def rrect(x0, y0, x1, y1, r, n=8):
    """A rectangle with round corners of radius r."""
    pts = []
    for cx, cy, a0 in ((x1 - r, y0 + r, -90), (x1 - r, y1 - r, 0),
                       (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180)):
        for i in range(n + 1):
            a = math.radians(a0 + 90 * i / n)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return ("poly", pts)


def ring(cx, cy, r_out, r_in):
    return ("ring", (cx, cy, r_out, r_in))


def bar(x0, y0, x1, y1, w):
    """A straight bar of width w from one point to another."""
    dx, dy = x1 - x0, y1 - y0
    k = w / 2 / math.hypot(dx, dy)
    nx, ny = -dy * k, dx * k
    return ("poly", [(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny),
                     (x0 - nx, y0 - ny)])


def ink(x0, y0, x1, y1, w=0.9):
    """A black line inside the shape, such as between a hand's fingers."""
    return ("ink", (x0, y0, x1, y1, w))


def rotate(parts, deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)

    def r(p):
        return (p[0] * c - p[1] * s, p[0] * s + p[1] * c)
    out = []
    for kind, g in parts:
        if kind == "poly":
            out.append((kind, [r(p) for p in g]))
        elif kind == "ring":
            out.append((kind, (*r(g[:2]), g[2], g[3])))
        else:
            out.append((kind, (*r(g[:2]), *r(g[2:4]), g[4])))
    return out


def double_arrow(half):
    """A two-headed arrow along the x axis, centred on the origin."""
    head = 4.2
    return [bar(-half + head - 0.5, 0, half - head + 0.5, 0, 1.6),
            poly((-half, 0), (-half + head, -head), (-half + head, head)),
            poly((half, 0), (half - head, -head), (half - head, head))]


# Each shape: its tablet number, its name, and how it is drawn at 640-canvas
# size and at 320. A size is drawn from parts and a hot spot (the point that
# is the pointer's position) at that size, or, where a shape is too small for
# the outlines to be smooth, as pixel art: rows of " " clear, "#" black, "o"
# white and "+" grey, with the hot spot's pixel.

def geo(parts, hot, inks=()):
    return ("geo", parts, hot, list(inks))


def art(rows, hot):
    return ("art", rows, hot)


def mirror(rows):
    return [r[::-1] for r in rows]


def transpose(rows):
    w = max(len(r) for r in rows)
    rows = [r.ljust(w) for r in rows]
    return ["".join(r[i] for r in rows) for i in range(w)]


EW_SMALL = ["  #     #  ",
            " #o#####o# ",
            "#ooooooooo#",
            " #o#####o# ",
            "  #     #  "]

NWSE_SMALL = ["#####    ",
              "#ooo#    ",
              "#oo#     ",
              "#o#o#    ",
              "## #o# ##",
              "    #o#o#",
              "     #oo#",
              "    #ooo#",
              "    #####"]

SHAPES = [
    (1, "arrow",
     geo([poly((0, 0), (0, 16), (4, 12.5), (6.6, 18.6), (9.2, 17.5), (6.6, 11.6),
               (11.4, 11.6))], (0.5, 0.5)),
     art(["#       ",
          "##      ",
          "#o#     ",
          "#oo#    ",
          "#ooo#   ",
          "#oooo#  ",
          "#ooooo# ",
          "#oooooo#",
          "#ooo####",
          "#o#o#   ",
          "## #o#  ",
          "   #o#  ",
          "    #   "], (0, 0))),
    (2, "crosshair",
     geo([rect(0, -8, 1, 9), rect(-8, 0, 9, 1)], (0.5, 0.5)),
     art(["   ###   ",
          "   #o#   ",
          "   #o#   ",
          "####o####",
          "#ooooooo#",
          "####o####",
          "   #o#   ",
          "   #o#   ",
          "   ###   "], (4, 4))),
    (3, "ibeam",
     geo([rect(0, -8, 1, 9), rect(-2, -9, 3, -8), rect(-2, 9, 3, 10)], (0.5, 0.5)),
     art(["#####",
          "#ooo#",
          "##o##",
          " #o# ",
          " #o# ",
          " #o# ",
          " #o# ",
          "##o##",
          "#ooo#",
          "#####"], (2, 5))),
    (4, "hand",
     geo([rrect(4, 0, 7.4, 12, 1.7),
          rrect(7.2, 6.4, 10.2, 12.6, 1.5),
          rrect(10, 7.2, 13, 13.2, 1.5),
          rrect(12.8, 8.4, 15.6, 13.6, 1.4),
          rrect(4, 10.2, 15.6, 19.4, 3.2),
          poly((0.6, 10.8), (2.6, 9.4), (5.6, 13.2), (5.4, 16.6))],
         (5.7, 0.5),
         [ink(7.3, 9.0, 7.3, 12.6), ink(10.1, 9.6, 10.1, 13.2), ink(12.9, 10.4, 12.9, 13.6)]),
     art(["   ##     ",
          "  #oo#    ",
          "  #oo#    ",
          "  #oo###  ",
          "  #oo#o###",
          "###oo#o#o#",
          "#oooo#o#o#",
          "#ooooooooo#",
          " #oooooooo#",
          "  #ooooooo#",
          "   #oooooo#",
          "   ########"], (3, 0))),
    (5, "resize-ew",
     geo([rect(-6, 0, 7, 1), poly((-10, 0.5), (-6, -3.5), (-6, 4.5)),
          poly((11, 0.5), (7, -3.5), (7, 4.5))], (0.5, 0.5)),
     art(EW_SMALL, (5, 2))),
    (6, "resize-ns",
     geo([rect(0, -6, 1, 7), poly((0.5, -10), (-3.5, -6), (4.5, -6)),
          poly((0.5, 11), (-3.5, 7), (4.5, 7))], (0.5, 0.5)),
     art(transpose(EW_SMALL), (2, 5))),
    (7, "resize-nwse",
     geo(rotate([rect(-5.5, -0.5, 5.5, 0.5), poly((-9.5, 0), (-5.5, -4), (-5.5, 4)),
                 poly((9.5, 0), (5.5, -4), (5.5, 4))], 45), (0, 0)),
     art(NWSE_SMALL, (4, 4))),
    (8, "resize-nesw",
     geo(rotate([rect(-5.5, -0.5, 5.5, 0.5), poly((-9.5, 0), (-5.5, -4), (-5.5, 4)),
                 poly((9.5, 0), (5.5, -4), (5.5, 4))], -45), (0, 0)),
     art(mirror(NWSE_SMALL), (4, 4))),
    (9, "move",
     geo([rect(-7, 0, 8, 1), rect(0, -7, 1, 8),
          poly((-11, 0.5), (-7, -3.5), (-7, 4.5)), poly((12, 0.5), (8, -3.5), (8, 4.5)),
          poly((0.5, -11), (-3.5, -7), (4.5, -7)), poly((0.5, 12), (-3.5, 8), (4.5, 8))],
         (0.5, 0.5)),
     art(["     #     ",
          "    #o#    ",
          "   #ooo#   ",
          "  # #o# #  ",
          " #o##o##o# ",
          "#ooooooooo#",
          " #o##o##o# ",
          "  # #o# #  ",
          "   #ooo#   ",
          "    #o#    ",
          "     #     "], (5, 5))),
    (10, "not-allowed",
     geo([ring(0.5, 0.5, 7.5, 5.3), bar(-3.2, -3.2, 4.2, 4.2, 2.0)], (0.5, 0.5)),
     art(["  #####  ",
          " #ooooo# ",
          "#oo###oo#",
          "#o#o###o#",
          "#o##o##o#",
          "#o###o#o#",
          "#oo###oo#",
          " #ooooo# ",
          "  #####  "], (4, 4))),
]

# ---- drawing -----------------------------------------------------------

def inside_poly(x, y, pts):
    c = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            c = not c
        j = i
    return c


def seg_dist(x, y, x0, y0, x1, y1):
    dx, dy = x1 - x0, y1 - y0
    if not dx and not dy:
        return math.hypot(x - x0, y - y0)
    t = ((x - x0) * dx + (y - y0) * dy) / (dx * dx + dy * dy)
    t = 0.0 if t < 0 else 1.0 if t > 1 else t
    return math.hypot(x - x0 - t * dx, y - y0 - t * dy)


def poly_dist(x, y, pts):
    d = 1e9
    j = len(pts) - 1
    for i in range(len(pts)):
        d = min(d, seg_dist(x, y, *pts[j], *pts[i]))
        j = i
    return d


def scaled(parts, k, dx, dy):
    out = []
    for kind, g in parts:
        if kind == "poly":
            out.append((kind, [(px * k + dx, py * k + dy) for px, py in g]))
        elif kind == "ring":
            out.append((kind, (g[0] * k + dx, g[1] * k + dy, g[2] * k, g[3] * k)))
        else:
            out.append((kind, (g[0] * k + dx, g[1] * k + dy, g[2] * k + dx, g[3] * k + dy,
                               max(g[4] * k, 0.8))))
    return out


def bounds(parts):
    xs, ys = [], []
    for kind, g in parts:
        if kind == "poly":
            xs += [p[0] for p in g]
            ys += [p[1] for p in g]
        elif kind == "ring":
            xs += [g[0] - g[2], g[0] + g[2]]
            ys += [g[1] - g[2], g[1] + g[2]]
    return min(xs), min(ys), max(xs), max(ys)


def classify(x, y, parts, inks):
    """2 for white, 1 for black (outline or ink), 0 for outside."""
    near = False
    for kind, g in parts:
        if kind == "poly":
            if inside_poly(x, y, g):
                for _, (x0, y0, x1, y1, w) in inks:
                    if seg_dist(x, y, x0, y0, x1, y1) <= w / 2:
                        return 1
                return 2
            if not near and poly_dist(x, y, g) <= OUTLINE:
                near = True
        else:
            cx, cy, r_out, r_in = g
            d = math.hypot(x - cx, y - cy)
            if r_in <= d <= r_out:
                return 2
            if not near and (r_out < d <= r_out + OUTLINE or r_in - OUTLINE <= d < r_in):
                near = True
    return 1 if near else 0


def render(parts, inks, hot, k):
    """The shape at scale k: its width, height, hot spot and pixels, each 0
    for clear or a palette entry from 1 (black) to 15 (white)."""
    parts = scaled(parts, k, 0, 0)
    inks = scaled(inks, k, 0, 0)
    x0, y0, x1, y1 = bounds(parts)
    pad = OUTLINE + 0.5
    ox, oy = math.floor(x0 - pad), math.floor(y0 - pad)
    w, h = math.ceil(x1 + pad) - ox, math.ceil(y1 + pad) - oy
    n = SAMPLES * SAMPLES
    pixels = []
    for py in range(h):
        for px in range(w):
            white = covered = 0
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    c = classify(ox + px + (sx + 0.5) / SAMPLES, oy + py + (sy + 0.5) / SAMPLES,
                                 parts, inks)
                    covered += c > 0
                    white += c == 2
            if covered * 2 <= n:
                pixels.append(0)
            else:
                pixels.append(1 + round(white / covered * (GREYS - 1)))
    # Trim clear rows and columns at the edges.
    rows = [pixels[r * w:(r + 1) * w] for r in range(h)]
    while rows and not any(rows[0]):
        rows.pop(0)
        oy += 1
    while rows and not any(rows[-1]):
        rows.pop()
    left = min(next(i for i, v in enumerate(r) if v) for r in rows if any(r))
    right = max(len(r) - next(i for i, v in enumerate(reversed(r)) if v) for r in rows if any(r))
    rows = [r[left:right] for r in rows]
    ox += left
    return {"w": right - left, "h": len(rows), "hot_x": math.floor(hot[0] - ox),
            "hot_y": math.floor(hot[1] - oy), "rows": rows}


ART_PIXELS = {" ": 0, "#": 1, "+": 1 + (GREYS - 1) // 2, "o": GREYS}


def render_art(rows, hot):
    w = max(len(r) for r in rows)
    return {"w": w, "h": len(rows), "hot_x": hot[0], "hot_y": hot[1],
            "rows": [[ART_PIXELS[c] for c in r.ljust(w)] for r in rows]}


def render_spec(spec):
    if spec[0] == "art":
        return render_art(spec[1], spec[2])
    return render(spec[1], spec[3], spec[2], 1.0)


def all_shapes():
    return [(number, name, render_spec(big), render_spec(small))
            for number, name, big, small in SHAPES]


# The greys, palette entries 1 (black) to 15 (white), as 5-bit levels.
def grey_levels():
    return [round(31 * i / (GREYS - 1)) for i in range(GREYS)]


# ---- output ------------------------------------------------------------

HEADER = """\
/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Generated by src/core/vga/mode/mode6.py -- do not edit.
 */
"""


def padded(size):
    """The rows of one size of a shape, as a mode 5 custom sprite holds
    them: its width and height padded up to a multiple of 4 with clear
    pixels on the right and at the bottom."""
    w, h = -(-size["w"] // 4) * 4, -(-size["h"] // 4) * 4
    rows = [r + [0] * (w - len(r)) for r in size["rows"]]
    return w, h, rows + [[0] * w] * (h - len(rows))


def pack(rows):
    """4 bits a pixel, the left one in the high nibble."""
    return [(r[i] << 4) | r[i + 1] for r in rows for i in range(0, len(r), 2)]


def emit_h(path, shapes):
    data = []
    entries = []
    max_bytes = 0
    for number, name, big, small in shapes:
        for size in (big, small):
            w, h, rows = padded(size)
            assert w <= 64 and h <= 64, f"{name} is too big for a custom sprite"
            packed = pack(rows)
            max_bytes = max(max_bytes, len(packed))
            entries.append((number, name, size, w, h, len(data)))
            data += packed
    lines = [HEADER, "#ifndef _MODE6_SHAPES_H_", "#define _MODE6_SHAPES_H_", "",
             "#include <stdint.h>", "",
             f"#define MODE6_SHAPE_COUNT {len(shapes) + 1}",
             "// The most bytes one size of a shape takes.",
             f"#define MODE6_MAX_BYTES {max_bytes}", "",
             "// The greys of palette entries 1 to 15, as 5-bit levels; entry 0 is clear.",
             "static const uint8_t MODE6_GREYS[16] = {",
             "    0, " + ", ".join(str(v) for v in grey_levels()) + ",", "};", "",
             "// The images of mode 5 custom sprites of 4 bits a pixel, the left pixel",
             "// in the high nibble.",
             f"static const uint8_t MODE6_DATA[{len(data)}] = {{"]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{v:02X}" for v in data[i:i + 16]) + ",")
    lines += ["};", "",
              "// Each shape at 640-canvas size, then at 320, by its tablet number:",
              "// its hot spot, the size byte of a mode 5 custom sprite, and where its",
              "// image is in MODE6_DATA.",
              "typedef struct",
              "{",
              "    uint8_t hot_x, hot_y, width_height;",
              "    uint16_t offset;",
              "} mode6_shape_t;", "",
              "static const mode6_shape_t MODE6_SHAPES[MODE6_SHAPE_COUNT][2] = {",
              "    {{0, 0, 0, 0}, {0, 0, 0, 0}}, // off"]
    by_number = {}
    for number, name, size, w, h, offset in entries:
        by_number.setdefault((number, name), []).append((size, w, h, offset))
    for (number, name), sizes in sorted(by_number.items()):
        cells = ", ".join("{%d, %d, 0x%02X, %d}" % (
            s["hot_x"], s["hot_y"], (h // 4 - 1) << 4 | (w // 4 - 1), o)
            for s, w, h, o in sizes)
        lines.append(f"    {{{cells}}}, // {number} {name}")
    lines += ["};", "", "#endif /* _MODE6_SHAPES_H_ */", ""]
    build.write_text(path, "\n".join(lines))


def grey_pixels():
    """The palette of the system's shapes as pixels: entry 0 clear, and
    each grey with the opaque bit, which mode6.c makes at init."""
    return [0] + [g | g << 6 | g << 11 | 0x20 for g in grey_levels()]


def emit_sv(path):
    pixels = ", ".join(f"16'h{v:04X}" for v in grey_pixels())
    lines = [HEADER, "package mode6_pkg;", "",
             "    /* The palette of the system's pointer shapes: entry 0 clear, then",
             "     * the greys, opaque. */",
             f"    localparam logic [15:0] MODE6_GREY [16] = '{{",
             f"        {pixels}",
             "    };", "",
             "endpackage", ""]
    build.write_text(path, "\n".join(lines))


def emit_png(path, shapes, scale=6):
    """Every shape, 640 size on a light and a dark band, then 320 size the
    same, each pixel scale times over, with its hot spot marked red."""
    cell = 32
    cols = len(shapes)
    width, height = cols * cell * scale, 4 * cell * scale
    rgb = bytearray(width * height * 3)
    bands = [(200, 200, 200), (40, 70, 110)]
    for b in range(4):
        color = bands[b % 2]
        for y in range(b * cell * scale, (b + 1) * cell * scale):
            for x in range(width):
                rgb[(y * width + x) * 3:(y * width + x) * 3 + 3] = bytes(color)
    levels = [0] + grey_levels()
    for i, (_, _, big, small) in enumerate(shapes):
        for b, size in enumerate((big, big, small, small)):
            k = scale if b < 2 else scale * 2
            x0 = i * cell * scale + (cell * scale - size["w"] * k) // 2
            y0 = b * cell * scale + (cell * scale - size["h"] * k) // 2
            for r, row in enumerate(size["rows"]):
                for c, v in enumerate(row):
                    if not v:
                        continue
                    g = levels[v] * 255 // 31
                    hot = r == size["hot_y"] and c == size["hot_x"]
                    pix = bytes((255, 0, 0) if hot else (g, g, g))
                    for yy in range(k):
                        for xx in range(k):
                            o = ((y0 + r * k + yy) * width + x0 + c * k + xx) * 3
                            rgb[o:o + 3] = pix
    build.write_png(path, width, height, rgb, 3)


def main():
    shapes = None

    def get():
        nonlocal shapes
        if shapes is None:
            shapes = all_shapes()
        return shapes
    args = build.generate(
        c={"mode6_shapes.h": lambda p: emit_h(p, get())},
        sv={"mode6_pkg.sv": emit_sv},
        extra=lambda ap: ap.add_argument("--png", metavar="FILE"))
    if args.png:
        emit_png(args.png, get())


if __name__ == "__main__":
    main()
