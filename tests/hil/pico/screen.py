# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""The console as it shows on the display, read from a capture device on the
VGA output: the character in each cell, and whether the cell is drawn
reversed. README.md has what the capture must be."""

import collections
import subprocess

import build

# The console's cells: 80 columns of the 8x16 font.
COLS, CELL_W, CELL_H = 80, 8, 16

# What is read, by the display type and the canvas, DISPLAY and CANVAS of
# the VGA's control channel: the display's pixels, and how many of them a
# pixel of the canvas takes each way. These are the ones captured on a rig:
# the console canvas on each display type, and the 640x360 canvas on a 16:9
# display, which shows it at 1280x720.
CANVAS_CONSOLE, CANVAS_640X360 = 0, 4
VIEWS = {(0, CANVAS_CONSOLE): (640, 480, 1),
         (1, CANVAS_CONSOLE): (640, 480, 1),
         (2, CANVAS_CONSOLE): (1280, 1024, 2),
         (1, CANVAS_640X360): (1280, 720, 2)}

# The display type as a chip keeps it: the VGA's own, or on the RIA the one
# it last sent the VGA. The canvas the same way.
DISPLAY_SYMBOLS = ("vga_display_selected", "vga_display_sent")
CANVAS_SYMBOLS = ("vga_canvas_selected", "vga_canvas_current")

# A cell that is no glyph: one under the pointer or the cursor.
UNREADABLE = "\ufffd"

# The most cells the pointer and the cursor cover between them.
COVERED_MAX = 16

# A stream starts with black frames. Frames are read until PICTURE_FRAMES
# have a picture, or WAIT_FRAMES have come, and the last FRAMES are kept.
FRAMES, PICTURE_FRAMES, WAIT_FRAMES = 8, 30, 600


class Screen:
    """The console as read(): `text`, a string for each row with a character
    for each cell, its code in the console's code page, or UNREADABLE;
    `reverse`, for each row whether each cell has the bright background of
    reverse video and of the selection; and `offset`, where the picture is in
    the capture, in pixels of the display right and down of true. The screen
    is read by brightness alone: text or a background much dimmer than the
    brightest thing on the screen, such as blue or red beside white, reads
    as a blank."""

    def __init__(self, text, reverse, offset):
        self.text = text
        self.reverse = reverse
        self.offset = offset

    def line(self, y):
        """Row `y` up to its last character. The blanks after it are left
        out, and so is the cursor there, which reads as nothing when it is
        lit."""
        return self.text[y].rstrip(" " + UNREADABLE)


class Font:
    """The glyphs of the 80-column font for a code page, as font.py builds
    them from src/core/term/font.c, each pixel `scale` pixels of the display
    each way."""

    def __init__(self, code_page, scale):
        import numpy as np
        font = build.load_module(build.ROOT / "src/core/term/font.py", "font")
        font16, _, pages, highs = font.build_tables(
            (build.ROOT / font.FONT_C).read_text(encoding="utf-8"))
        rows = np.array(font16[:CELL_H * 256], np.uint8).reshape(CELL_H, 256)
        if code_page in pages:
            high = highs[pages.index(code_page)][0]
            rows[:, 128:] = np.array(high, np.uint8).reshape(CELL_H, 128)
        pixels = np.unpackbits(rows.T[:, :, None], axis=2)
        pixels = pixels.repeat(scale, axis=1).repeat(scale, axis=2)
        # Of glyphs that look the same, printable ASCII is the one read.
        order = list(range(0x20, 0x7F)) + list(range(0x20)) + list(range(0x7F, 0x100))
        self.glyphs = [(np.packbits(pixels[code], axis=1), chr(code)) for code in order]
        self.tables = {}

    def match(self, pixels, known):
        """The character and whether it is reversed for a cell's pixels,
        which cells_at() packs, or None. Only the pixels `known` are
        compared. A cell that is a glyph both ways is the printable ASCII
        one, so a bright blank is a reversed space."""
        mask = known.tobytes()
        if mask not in self.tables:
            table = self.tables[mask] = {}
            for glyph, char in self.glyphs:
                table.setdefault((glyph & known).tobytes(), char)
        table = self.tables[mask]
        ways = [(table.get((pixels & known).tobytes()), False),
                (table.get((~pixels & known).tobytes()), True)]
        ways = [w for w in ways if w[0]]
        return next((w for w in ways if " " <= w[0] <= "~"), ways[0] if ways else None)


def console_view(probe):
    """What the chip on `probe` shows the console as, from its RAM: the
    display's width and height, the display pixels a canvas pixel takes, the
    console's rows, its code page, and the display row its first row is on. The probe is on the RIA or the VGA,
    with the ELF of its firmware. Raises for what read() does not read: a
    display type and canvas not in VIEWS, and a terminal that is not 80
    columns wide and all on the canvas."""
    names = [next((n for n in symbols if n in probe.sym), None)
             for symbols in (DISPLAY_SYMBOLS, CANVAS_SYMBOLS)]
    if None in names:
        raise RuntimeError("the screen is read at the size the board says, which "
                           "needs the ELF of the firmware on a probe: --elf or --vga-elf")
    display, canvas = (probe.read(probe.sym[n], 8, 1)[0] for n in names)
    if (display, canvas) not in VIEWS:
        raise RuntimeError(f"canvas {canvas} on display type {display} (SET VGA {display}) "
                           "has not been captured on a rig, so the screen is not read "
                           "on it")
    width, height, scale = VIEWS[display, canvas]
    # vterm_placement_t in src/core/term/vterm.h.
    begin, begin_high, cols, rows, row_px, console, placed = probe.read(
        probe.sym["vterm_placement"], 8, 7)
    begin |= begin_high << 8
    if (not placed or cols != COLS or row_px != CELL_H or not rows
            or (begin + rows * CELL_H) * scale > height):
        raise RuntimeError("the terminal is not placed as the screen is read, 80 columns "
                           "on the canvas: a program has placed it otherwise, or the ELF "
                           f"is not the firmware on the chip. It has {cols} columns and "
                           f"{rows} rows of {row_px} scanlines from scanline {begin}, "
                           f"placed {placed}")
    return (width, height, scale, rows, probe.read(probe.sym["vterm_code_page"], 16, 1)[0],
            begin * scale)


def sizes(device):
    """The sizes a capture device has in YUYV, as ffmpeg lists them."""
    listing = subprocess.run(
        ["ffmpeg", "-hide_banner", "-f", "v4l2", "-list_formats", "all", "-i", device],
        capture_output=True, text=True).stderr
    found = []
    for line in listing.splitlines():
        if "yuyv422" in line:
            found += [tuple(map(int, s.split("x"))) for s in line.rsplit(":", 1)[1].split()
                      if s.replace("x", "").isdigit()]
    if not found:
        raise RuntimeError(f"{device} lists no YUYV sizes:\n{listing[-500:]}")
    return found


def capture_size(have, width, height):
    """The size to capture a display at: its own, or the smallest whole
    multiple of it the device has."""
    whole = sorted((w, h) for w, h in have
                   if w % width == 0 and h % height == 0 and w // width == h // height)
    if not whole:
        raise RuntimeError(
            f"the capture device has no size that is {width}x{height} or a whole "
            "multiple of it, so its picture cannot be read pixel for pixel; it has "
            + " ".join(f"{w}x{h}" for w, h in sorted(set(have))))
    return whole[0]


def grab(device, width, height):
    """The mean luma of FRAMES frames from the device at a size. YUYV has a
    luma for every pixel and a color for every two, so the luma is what is
    read."""
    import numpy as np
    size = width * height * 2
    proc = subprocess.Popen(
        ["ffmpeg", "-loglevel", "error", "-f", "v4l2", "-input_format", "yuyv422",
         "-video_size", f"{width}x{height}", "-framerate", "60", "-i", device,
         "-f", "rawvideo", "-pix_fmt", "yuyv422", "-"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    kept, pictures = collections.deque(maxlen=FRAMES), 0
    try:
        for _ in range(WAIT_FRAMES):
            buf = proc.stdout.read(size)
            if len(buf) < size:
                break
            luma = np.frombuffer(buf, np.uint8)[::2]
            if int(luma.max()) - int(luma.min()) > 64:
                kept.append(luma)
                pictures += 1
                if pictures == PICTURE_FRAMES:
                    break
    finally:
        proc.kill()
        said = proc.communicate()[1].decode("ascii", "replace").strip()
    if pictures < FRAMES:
        raise RuntimeError(f"{device} gave {pictures} frames with a picture at "
                           f"{width}x{height}: the display is black, or the device "
                           "has no signal" + (f": {said[-300:]}" if said else ""))
    return np.mean(kept, axis=0).reshape(height, width)


def cells_at(luma, rows, scale, zoom, ox, oy):
    """The console's cells with one capture pixel for each pixel of the
    display, the first at ox, oy and the rest every `zoom`. For each cell:
    whether it has ink; its pixels packed into bytes, a bit set where the
    pixel is the brighter of the cell's two levels, or in a cell of one
    level where that level is bright; and packed the same way, which pixels
    the capture has. It has none for a pixel off its edge, as the last
    column is when the picture sits to the right, nor for the display's
    outermost pixels in a capture that is scaled up."""
    import numpy as np
    height, width = luma.shape
    ys = np.arange(rows * CELL_H * scale) * zoom + oy
    xs = np.arange(COLS * CELL_W * scale) * zoom + ox
    # A device that scales the picture up does not scale its outermost
    # pixels as it does the rest, so those are not read either.
    edge = zoom if zoom > 1 else 0
    known = np.logical_and.outer((ys >= edge) & (ys < height - edge),
                                 (xs >= edge) & (xs < width - edge))
    pixels = np.where(known, luma[np.ix_(ys.clip(0, height - 1), xs.clip(0, width - 1))],
                      np.nan)

    def by_cell(a):
        return a.reshape(rows, CELL_H * scale, COLS, CELL_W * scale).transpose(0, 2, 1, 3)
    pixels, known = by_cell(pixels), by_cell(known)
    low = np.nanmin(pixels, axis=(2, 3), keepdims=True)
    high = np.nanmax(pixels, axis=(2, 3), keepdims=True)
    dark, bright = luma.min(), luma.max()
    inked = high - low > (bright - dark) / 3
    lit = pixels > np.where(inked, (low + high) / 2, (dark + bright) / 2)
    return inked[:, :, 0, 0], np.packbits(lit, axis=3), np.packbits(known, axis=3)


def read(luma, view, font):
    """The Screen in a capture's luma, for a view of console_view() and the
    Font of its code page and scale. The capture is the display's size or a
    whole multiple of it. Converters put the picture up to a pixel of the
    display from true, so every place from one pixel left and up of true to
    one right and down is tried, and the first where the most cells are
    glyphs is read. Raises when none is good, which is the capture's fault
    and not the console's."""
    width, height, scale, rows, _, top = view
    zoom = luma.shape[1] // width
    if luma.shape != (zoom * height, zoom * width):
        raise RuntimeError(f"a capture of {luma.shape[1]}x{luma.shape[0]} is not a "
                           f"whole multiple of the display's {width}x{height}")
    best = None
    for oy in range(-zoom, zoom + 1):
        for ox in range(-zoom, zoom + 1):
            inked, pixels, known = cells_at(luma, rows, scale, zoom, ox, oy + top * zoom)
            found = [[font.match(pixels[y, x], known[y, x]) for x in range(COLS)]
                     for y in range(rows)]
            glyphs = sum(bool(found[y][x]) for y, x in zip(*inked.nonzero()))
            others = int(inked.sum()) - glyphs
            if not best or (glyphs, -others) > (best[0], -best[1]):
                best = (glyphs, others, ox, oy, found)
    glyphs, others, ox, oy, found = best
    if not glyphs:
        raise RuntimeError(
            "no cell of the capture is a glyph of the font, so there is no text to "
            "find the picture's place by: the screen has no text on it, or the "
            "capture is not the console's picture")
    if others > COVERED_MAX:
        raise RuntimeError(
            "the capture is not the console's picture at any place within a pixel of "
            f"true: at best {others} cells with ink are no glyph of the font and "
            f"{glyphs} are one. Unless the screen shows something other than text, the "
            "fault is in the capture: the device, its scaling, or the converter "
            "before it.")
    return Screen(["".join(c[0] if c else UNREADABLE for c in row) for row in found],
                  [[bool(c and c[1]) for c in row] for row in found],
                  (ox / zoom, oy / zoom))


class Camera:
    """A capture device on the VGA output, and the probe that says what the
    board is showing there. It is made and used inside the probe's `with`."""

    def __init__(self, device, probe):
        self.device = device
        self.probe = probe
        # A board that is not read is refused here, before a test does anything.
        console_view(probe)
        self.sizes = sizes(device)
        self.fonts = {}

    def look(self):
        """The Screen now."""
        view = console_view(self.probe)
        cut = (view[4], view[2])
        if cut not in self.fonts:
            self.fonts[cut] = Font(*cut)
        luma = grab(self.device, *capture_size(self.sizes, view[0], view[1]))
        return read(luma, view, self.fonts[cut])
