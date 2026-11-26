# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""The rig of the hardware tests: an RP6502-PICO, its console, and a debug
probe on the SWD port of the RIA, the VGA, or each. The console, the probes
and the flashing are src/core/build.py's; what is here is the tests' own."""

import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "src" / "core"))
sys.dont_write_bytecode = True

import build  # noqa: E402

# clk_sys of the RIA, the cycles that the audio interrupt counts in.
CLK_SYS_HZ = 256_000_000
# The rate of the audio interrupt, one OPL2 sample of 72 master clocks.
AUD_RATE = 49716


class Probe(build.Probe):
    """build.Probe on the RIA, with the audio load the tests read."""

    LOAD_FIELDS = ("second", "sum", "peak", "overruns", "peak_max", "pair",
                   "pair_max")

    def watch(self, stop, xram_at=None):
        """Reads aud_load, and the XRAM byte at `xram_at`, until `stop` is
        set. Returns a dict for each second that the audio interrupt
        published, without the first, which can start before the ROM."""
        rows = {}
        while not stop.is_set():
            v = self.read(self.sym["aud_load"], 32, len(self.LOAD_FIELDS))
            row = dict(zip(self.LOAD_FIELDS, v))
            if xram_at is not None:
                row["xram"] = self.read(self.sym["xram"] + xram_at, 8, 1)[0]
            rows.setdefault(row["second"], row)
            stop.wait(0.25)
        return list(rows.values())[1:]


class VgaProbe(build.Probe):
    """build.Probe on the VGA, with what the terminal tests read after a
    run: the console's bytes that came while a job of the terminal was
    pending (draw_bytes_in_jobs in src/core/term/draw.h), and the overrun
    flag of the PIX register FIFO, which a word the VGA had no room for sets:
    RXSTALL of state machine 1 of PIO1 (src/host/pico/vga/sys/pix.h) in its
    FDEBUG, which a write of 1 clears."""

    FDEBUG = 0x50300008
    PIX_REGS_RXSTALL = 1 << 1

    def clear(self):
        """Both back to zero, before a run."""
        self.write(self.sym["draw_bytes_in_jobs"], 32, [0])
        self.write(self.FDEBUG, 32, [self.PIX_REGS_RXSTALL])

    def faults(self):
        """The bytes that came during a job, and whether the FIFO overran."""
        late = self.read(self.sym["draw_bytes_in_jobs"], 32, 1)[0]
        overrun = bool(self.read(self.FDEBUG, 32, 1)[0] & self.PIX_REGS_RXSTALL)
        return late, overrun


def selection(probe):
    """The console's selection as a chip keeps it, vterm_selection
    (src/core/term/screen.h), read through its probe: whether it shows, the
    grid it is on with the screen's place in that grid, its ends, rows of
    memory and columns, and the rows of memory it takes. The grid's layout is
    the same on the RIA and the VGA, so the places compare. Only what shows
    is compared: the ends of one dropped stay as they were."""
    w = probe.read(probe.sym["vterm_selection"], 32, 5)
    shows = bool(w[4] & 0xFF)
    if not shows:
        return {"shows": False}
    screen = None
    for grid in ("vterm_40", "vterm_80"):
        if 0 <= w[2] - probe.sym[grid] < 512:
            screen = (grid, w[2] - probe.sym[grid])
    return {"shows": True, "screen": screen,
            "ends": tuple((w[3] >> s) & 0xFF for s in (0, 8, 16, 24)),
            "rows": w[0] | w[1] << 32}


def console(probe):
    """The console's terminal as a chip keeps it, read through its probe:
    for each size, vterm_40 and vterm_80 (src/core/term/vterm.h), the
    grid's height and which screen shows, and for each of its two screens
    the cursor, the saved cursor, and the ring its rows are laid out in
    (screen_t in src/core/term/screen.h): the slot of row 0, the rows above
    it still in the ring, the spare row of memory, and the row of memory in
    each slot. The RIA's and the VGA's are the same after the same bytes,
    which is what lets the RIA name a row of memory to the VGA.

    The places read are those of term_grid_t and vterm_size_t in a 32-row
    build: the width and height first, the screens of 24 bytes from 8, the
    screen showing at 0x38, and the slots at 0xA2. A build that moves them
    reads as a ring of another size, and raises."""
    out = {}
    for name, width in (("vterm_40", 40), ("vterm_80", 80)):
        base = probe.sym[name]
        b = bytes(probe.read(base, 8, 0xA2 + 64))
        grid = {"height": b[1],
                "shown": (int.from_bytes(b[0x38:0x3C], "little") - base - 8) // 24}
        for n in (0, 1):
            s = b[8 + 24 * n:32 + 24 * n]
            if b[0] != width or s[6] != 32 or s[7] != 33:
                raise RuntimeError(f"{name} is not laid out as rig.console() reads it")
            grid[f"screen {n}"] = {"cursor": tuple(s[0:3]), "saved": tuple(s[3:6]),
                                   "row 0 in slot": s[8], "hidden": s[9], "spare": s[10],
                                   "slots": tuple(b[0xA2 + 32 * n:0xA2 + 32 * n + 32])}
        out[name] = grid
    return out


def console_differences(ria, vga):
    """What differs between two chips' console() as lines of text, none when
    they are the same."""
    lines = []
    for name in ria:
        for key, a in ria[name].items():
            b = vga[name][key]
            if not isinstance(a, dict):
                a, b = {key: a}, {key: b}
                key = ""
            lines += [f"{name} {key} {k}: the RIA has {a[k]}, the VGA {b[k]}".replace("  ", " ")
                      for k in a if a[k] != b[k]]
    return lines


def load_summary(rows):
    """One line of the audio load from the rows of Probe.watch()."""
    if not rows:
        return "audio load: no whole second sampled"
    avg = sum(r["sum"] for r in rows) / len(rows) / CLK_SYS_HZ * 100
    budget = CLK_SYS_HZ / AUD_RATE
    peak = max(r["peak"] for r in rows)
    return (f"audio load {avg:.1f}% over {len(rows)} s, peak sample "
            f"{peak} cycles ({peak / budget * 100:.0f}%), "
            f"{sum(r['overruns'] for r in rows)} overruns")


class LateLines:
    """Watches the VGA output on a capture device while the block runs and
    counts the frames with a late scanline: a row drawn in scanvideo's
    missing-line blue across an eighth of the screen or more. A late
    scanline can be blue across part of the row only, such as its left
    half, and the capture blends it with the rows beside it. Every frame is checked, so
    the capture must keep up at the display's rate. The test's own colors
    must not include that blue."""

    WIDTH, HEIGHT = 1280, 1024

    def __init__(self, device):
        self.device = device
        self.frames = 0
        self.late = 0
        self.first = None

    def __enter__(self):
        import threading
        self.proc = subprocess.Popen(
            ["ffmpeg", "-loglevel", "error", "-f", "v4l2", "-input_format", "yuyv422",
             "-video_size", f"{self.WIDTH}x{self.HEIGHT}", "-framerate", "60",
             "-i", self.device, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.thread = threading.Thread(target=self._run)
        self.thread.start()
        return self

    def _run(self):
        import numpy as np
        size = self.WIDTH * self.HEIGHT * 3
        while True:
            buf = self.proc.stdout.read(size)
            if len(buf) < size:
                return
            a = np.frombuffer(buf, np.uint8).reshape(self.HEIGHT, self.WIDTH, 3)
            a = a.astype(np.int16)
            blue = (a[:, :, 2] > 100) & (a[:, :, 2] > a[:, :, 0] + 50) & (a[:, :, 2] > a[:, :, 1] + 50)
            rows = np.count_nonzero(blue.sum(axis=1) > self.WIDTH // 8)
            self.frames += 1
            if rows:
                self.late += 1
                if self.first is None:
                    self.first = (self.frames, rows)

    def __exit__(self, *_):
        self.proc.terminate()
        self.thread.join()
        self.proc.wait()
