#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""A pipe that ends without a newline, read with no window. The program copies
each line to stdout and writes "eof" at end of file, so the unfinished line
still arrives, though nothing answered the line editor's questions."""

import subprocess

import rp6502
from rp6502_rom import main


def test_headless_eof():
    r = subprocess.run(rp6502.Emu.command("--headless"), input="bb",
                       capture_output=True, text=True, timeout=60)
    assert r.stdout == "bb\neof\n", f"stdout was {r.stdout!r}"


TESTS = {"headless_eof": test_headless_eof}
main(globals())
