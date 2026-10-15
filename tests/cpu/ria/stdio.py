#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import subprocess
import sys

from rp6502_asm import (API_A, API_X, OP_EXIT, OP_READ_XSTACK, OP_WRITE_XSTACK,
                        XSTACK, Asm)
from rp6502_rom import command, main, rom, script

EXIT_CODE = 3
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
expect-exit EXIT_CODE
expect-not "\neof"
expect "eof\n"
"""


def test_eof_empty():
    script(EOF_EMPTY_SCRIPT, rom("stdio"))


def test_stdio():
    r = subprocess.run(command("--headless", "--phi2", "0", rom("stdio")),
                       input=INPUT, capture_output=True, text=True, timeout=60)
    ok = True
    for name, got, want in (("stdout", r.stdout, OUTPUT),
                            ("stderr", r.stderr, ERR),
                            ("exit code", r.returncode, EXIT_CODE)):
        if got != want:
            print(f"{sys.argv[0]}: {name} {got!r}, expected {want!r}",
                  file=sys.stderr)
            ok = False
    return 0 if ok else 1


ROMS = {"stdio": prog}
TESTS = {"eof_empty": test_eof_empty, "stdio": test_stdio}
main(globals())
