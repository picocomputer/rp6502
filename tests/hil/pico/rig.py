# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""The rig of the hardware tests: an RP6502-PICO, its console, and a debug
probe on the SWD port of the RIA. The console, the probe and the flashing
are src/core/build.py's; what is here is the tests' own."""

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
    missing-line blue across most of the screen. Every frame is checked, so
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
            blue = (a[:, :, 2] > 150) & (a[:, :, 0] < 90) & (a[:, :, 1] < 90)
            rows = np.count_nonzero(blue.sum(axis=1) > self.WIDTH // 2)
            self.frames += 1
            if rows:
                self.late += 1
                if self.first is None:
                    self.first = (self.frames, rows)

    def __exit__(self, *_):
        self.proc.terminate()
        self.thread.join()
        self.proc.wait()
