#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""A stopped program's canvas goes back to the console on the next frame.
The program draws on a 320x240 canvas and the console's is 640x480, and shot
writes a PNG the size of the canvas, so the PNG's header says which is up."""

import struct

import rp6502
from rp6502_rom import main, rom, save_dir


def size(png):
    with open(png, "rb") as f:
        return struct.unpack(">II", f.read(24)[16:24])


def test_canvas():
    here = save_dir()
    with rp6502.Emu(rom("mode3_1bpp")) as e:
        e.cmd("run 20")
        e.cmd(f'shot "{(here / "drawn.png").as_posix()}"')
        e.cmd("sys stop")
        e.cmd("run 1")
        e.cmd(f'shot "{(here / "stopped.png").as_posix()}"')
    for png, want in (("drawn.png", (320, 240)), ("stopped.png", (640, 480))):
        got = size(here / png)
        assert got == want, \
            f"{png} is {got[0]}x{got[1]}, expected {want[0]}x{want[1]}"


TESTS = {"canvas": test_canvas}
main(globals())
