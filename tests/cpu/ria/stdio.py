#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import subprocess

import rp6502
from rp6502_asm import (API_A, API_X, OP_CLOSE, OP_EXIT, OP_READ_XSTACK,
                        OP_WRITE_XSTACK, XSTACK, Asm)
from rp6502_rom import main, rom, script

EXIT_CODE = 3
EXIT = 0x10  # zero page byte that holds the OR of the close results
ERR = "err\n"
EOF = "eof\n"
INPUT = "a\rbb\r"
# core/api/std.c follows each line read from stdin with a newline, whether a
# carriage return or a line feed ended the line in the input.
OUTPUT = "a\nbb\n" + EOF


def write_str(p, fd, s):
    for c in reversed(s.encode("latin-1")):
        p.push(c)
    p.call_a(OP_WRITE_XSTACK, fd)


def prog():
    p = Asm()
    write_str(p, 2, ERR)

    p.symbol("next")
    p.push(0)
    p.push(1)
    p.call_a(OP_READ_XSTACK, 0)
    p.tax()
    with p.branch("beq"):
        p.lda_abs(XSTACK)
        p.sta_abs(XSTACK)
        p.call_a(OP_WRITE_XSTACK, 1)
        p.jmp_abs("next")
    write_str(p, 1, EOF)
    p.store(API_A, EXIT_CODE)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()
    return p


EOF_EMPTY_SCRIPT = r"""
# The program copies each line typed to stdout, and on end of file it writes
# "eof" and exits with EXIT_CODE. Nothing is typed, so the read that is
# waiting gets no bytes when stdin ends, not an empty line.

run 10
eof
wait exit
expect exit EXIT_CODE
expect-not "\neof"
expect "eof\n"
"""


def test_eof_empty():
    script(EOF_EMPTY_SCRIPT, rom("stdio"))


def test_stdio():
    cmd = rp6502.Emu.command("--headless", "--phi2", "0", rom("stdio"))
    r = subprocess.run(cmd, input=INPUT, capture_output=True, text=True,
                       timeout=60)
    for name, got, want in (("stdout", r.stdout, OUTPUT),
                            ("stderr", r.stderr, ERR),
                            ("exit code", r.returncode, EXIT_CODE)):
        assert got == want, f"{name} {got!r}, expected {want!r}"


def prog_close():
    """Closes stdin, stdout, stderr, CON: and TTY:, and exits with the OR of
    the five results, then writes to stdout to show that it is still open."""
    p = Asm()
    p.store(EXIT, 0)
    for fd in range(5):
        p.call_a(OP_CLOSE, fd)
        p.ora_abs(EXIT)
        p.sta_abs(EXIT)
    write_str(p, 1, "ok\n")
    p.lda_abs(EXIT)
    p.sta_abs(API_A)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()
    return p


CLOSE_SCRIPT = r"""
# A close of a console descriptor succeeds and leaves it open.

wait exit
expect exit 0
expect "ok\n"
"""


def test_close_console():
    script(CLOSE_SCRIPT, rom("close"))


ROMS = {"stdio": prog, "close": prog_close}
TESTS = {"eof_empty": test_eof_empty, "stdio": test_stdio,
         "close_console": test_close_console}
main(globals())
