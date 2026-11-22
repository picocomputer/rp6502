#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""The host's terminal on a pty, as the emulator's console. The line editor
takes the prompt column and the size from term.c, so the host's terminal is
never asked anything and gets no cursor shape, with a window or without, and
the line editor breaks its rows at term.c's 80 columns. With stdout a pipe,
the line editor types plainly. The program writes "err" to
stderr, then copies each line typed to stdout, and the line editor echoes what
is typed."""

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

QUERIES = (b"\x1b[6n", b"\x1b[999;999H", b"\x1b[>c", b" q")


def controlling_terminal():
    """The pty becomes the emulator's controlling terminal, as a shell's is,
    so the emulator is in its foreground group and may put it in raw mode."""
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


def session(*args, line=b"bb"):
    """Runs the emulator on a pty, types line and Enter once the program has
    started, and returns what the pty got up to the program's copy of the
    line. The emulator's CR LF comes out as CR CR LF, because the pty's output
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
        while not re.search(re.escape(line) + rb"\r*\n", out):
            assert time.monotonic() < deadline, f"the pty got {out!r}"
            if select.select([host], [], [], 0.05)[0]:
                out += os.read(host, 4096)
            if not typed and b"err" in out:
                os.write(host, line + b"\r")
                typed = True
    finally:
        emu.kill()
        emu.wait()
        os.close(host)
    return out


def test_console_headless_terminal():
    out = session("--headless", line=b"b" * 100)
    for query in QUERIES:
        assert query not in out, f"{query!r} was sent: {out!r}"
    assert re.search(rb"(?<!b)b{80}\r+\nb{20}\r+\n", out), \
        f"the echo did not break at 80: {out!r}"


def test_console_headless_pipe():
    """Typed on a pty with stdout a pipe: the backspaces edit the line, and
    stdout gets the program's line and nothing else."""
    host, term = pty.openpty()
    emu = subprocess.Popen(rp6502.Emu.command("--headless"), stdin=term,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           preexec_fn=controlling_terminal)
    os.close(term)
    out = b""
    err = b""
    typed = False
    try:
        deadline = time.monotonic() + 30
        while b"\n" not in out:
            assert time.monotonic() < deadline, f"stdout got {out!r}"
            ready = select.select([emu.stdout, emu.stderr], [], [], 0.05)[0]
            if emu.stdout in ready:
                out += os.read(emu.stdout.fileno(), 4096)
            if emu.stderr in ready:
                err += os.read(emu.stderr.fileno(), 4096)
            if not typed and b"err" in err:
                os.write(host, b"bc\x7f\x7fab\r")
                typed = True
    finally:
        emu.kill()
        emu.wait()
        os.close(host)
    assert out == b"ab\n", f"stdout got {out!r}"


def test_console_window():
    out = session("--screenshot", "shot.png", "--frames", "1000000", "--stdin")
    for query in QUERIES:
        assert query not in out, f"{query!r} was sent: {out!r}"


TESTS = {
    "console_headless_terminal": test_console_headless_terminal,
    "console_headless_pipe": test_console_headless_pipe,
    "console_window": test_console_window,
}
main(globals())
