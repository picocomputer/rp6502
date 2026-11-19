#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import subprocess

import rp6502
from rp6502_asm import (API_A, API_X, O_RDWR, OP_CLOSE, OP_EXIT, OP_OPEN,
                        OP_READ_XSTACK, OP_WRITE_XSTACK, XSTACK, Asm)
from rp6502_rom import main, rom, script

EXIT_CODE = 3
EXIT = 0x10  # zero page byte that holds the OR of the close results
ERR = "err\n"
EOF = "eof\n"
INPUT = "a\rbb\r"
# core/com/console.c follows each line read from stdin with a newline, whether a
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
    """Closes stdout, then shows it closed: a write to 1 fails, 3 and 4 were
    never open, and an open of CON:1 lands on 1, the lowest free descriptor,
    where it writes "ok". Exits with the OR of what each step got wrong."""
    p = Asm()
    p.store(EXIT, 0)

    def wrong_unless(want):
        # A is the low byte of the result; anything but want is an error.
        p.cmp_imm(want)
        with p.branch("beq"):
            p.inc_abs(EXIT)

    p.call_a(OP_CLOSE, 1)
    wrong_unless(0)
    write_str(p, 1, "lost\n")
    wrong_unless(0xFF)
    for fd in (3, 4):
        p.call_a(OP_CLOSE, fd)
        wrong_unless(0xFF)
    p.push_str("CON:1")
    p.call_a(OP_OPEN, O_RDWR)
    wrong_unless(1)
    write_str(p, 1, "ok\n")
    p.lda_abs(EXIT)
    p.sta_abs(API_A)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()
    return p


CLOSE_SCRIPT = r"""
# close() of a console descriptor really closes it, and the console opens
# again on the lowest free descriptor.

wait exit
expect exit 0
expect-not "lost"
expect "ok\n"
"""


def test_close_console():
    script(CLOSE_SCRIPT, rom("close"))


ROMS = {"stdio": prog, "close": prog_close}
TESTS = {"eof_empty": test_eof_empty, "stdio": test_stdio,
         "close_console": test_close_console}
main(globals())
