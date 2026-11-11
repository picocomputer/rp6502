#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""The host's terminal on a pty, as the emulator's console. With no window it
is the terminal in charge, and the line editor asks it for the prompt column,
DA2 and the size. With a window, term.c is in charge and the host's terminal
is never asked anything. The program writes "err" to stderr, then copies each
line typed to stdout, and the line editor echoes what is typed."""

import fcntl
import os
import pty
import re
import select
import subprocess
import termios
import time

import rp6502
from rp6502_rom import main

QUERIES = (b"\x1b[6n", b"\x1b[>c", b"\x1b[999;999H", b" q")


def controlling_terminal():
    """The pty becomes the emulator's controlling terminal, as a shell's is,
    so the emulator is in its foreground group and may put it in raw mode."""
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


def session(*args):
    """Runs the emulator on a pty, types "bb" and Enter once the program has
    started, and returns what the pty got up to the echo of the line. The
    emulator's CR LF comes out as CR CR LF, because the pty's output
    processing turns each LF into CR LF."""
    host, term = pty.openpty()
    emu = subprocess.Popen(rp6502.Emu.command(*args), stdin=term, stdout=term,
                           stderr=subprocess.DEVNULL,
                           preexec_fn=controlling_terminal)
    os.close(term)
    out = b""
    typed = False
    try:
        deadline = time.monotonic() + 30
        while not re.search(rb"bb\r*\n", out):
            assert time.monotonic() < deadline, f"the pty got {out!r}"
            if select.select([host], [], [], 0.05)[0]:
                out += os.read(host, 4096)
            if not typed and b"err" in out:
                os.write(host, b"bb\r")
                typed = True
    finally:
        emu.kill()
        emu.wait()
        os.close(host)
    return out


def test_console_headless_terminal():
    out = session("--headless")
    for query in QUERIES[:3]:
        assert query in out, f"{query!r} was not asked: {out!r}"


def test_console_window():
    out = session("--screenshot", "shot.png", "--frames", "1000000", "--stdin")
    for query in QUERIES:
        assert query not in out, f"{query!r} was sent: {out!r}"


TESTS = {
    "console_headless_terminal": test_console_headless_terminal,
    "console_window": test_console_window,
}
main(globals())
