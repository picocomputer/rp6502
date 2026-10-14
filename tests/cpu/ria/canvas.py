#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""A stopped program's canvas goes back to the console on the next frame.
The program draws on a 320x240 canvas and the console's is 640x480, and shot
writes a PNG the size of the canvas, so the PNG's header says which is up."""

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "gen"))
import rp6502_script  # noqa: E402


def size(png):
    with open(png, "rb") as f:
        return struct.unpack(">II", f.read(24)[16:24])


def drive(emu, rom, save_dir):
    here = Path(save_dir).resolve()

    def body(e):
        e.cmd("run 20")
        e.cmd(f'shot "{(here / "drawn.png").as_posix()}"')
        e.cmd("sys stop")
        e.cmd("run 1")
        e.cmd(f'shot "{(here / "stopped.png").as_posix()}"')
    code = rp6502_script.drive(emu, rom, body, save_dir=save_dir)
    if code:
        return code
    for png, want in (("drawn.png", (320, 240)), ("stopped.png", (640, 480))):
        got = size(here / png)
        if got != want:
            print(f"{sys.argv[0]}: {png} is {got[0]}x{got[1]}, expected "
                  f"{want[0]}x{want[1]}", file=sys.stderr)
            return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--drive", action="store_true", help="run the check")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="mode3_1bpp.rp6502")
    ap.add_argument("--save-dir", help="the folder behind SAVE:")
    a = ap.parse_args()
    if a.drive:
        return drive(a.emu, a.rom, a.save_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
