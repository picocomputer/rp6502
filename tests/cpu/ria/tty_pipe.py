#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# INPUT contains a 0x03. The emulator reads stdin on a pipe as a stream, where
# 0x03 is an ordinary byte that raises no SIGINT, so the SIGINT attribute is
# still clear when the program reads it before exiting.

import subprocess

import rp6502
from rp6502_asm import (API_A, API_X, OP_ATTR_GET, OP_CLOSE, OP_EXIT, OP_OPEN,
                        OP_READ_XSTACK, OP_WRITE_XSTACK, O_RDONLY, XSTACK,
                        Asm)
from rp6502_rom import main, rom

ATTR_SIGINT = 0x08
FD = 0x0200
INPUT = "raw \x03 bytes."
END = "."
EXIT_CODE = 7
EXIT_SIGINT = 9


def prog():
    p = Asm()
    p.push_str("TTY:")
    p.lda_imm(O_RDONLY)
    p.sta_abs(API_A)
    p.call(OP_OPEN)
    p.sta_abs(FD)

    p.symbol("poll")
    p.push(0)
    p.push(1)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)
    p.tax()
    with p.branch("beq"):
        p.lda_abs(XSTACK)
        p.sta_abs(0x0201)
        p.sta_abs(XSTACK)
        p.call_a(OP_WRITE_XSTACK, 1)
        p.lda_abs(0x0201)
        p.cmp_imm(ord(END))
        with p.branch("bne"):
            p.lda_abs(FD)
            p.sta_abs(API_A)
            p.call(OP_CLOSE)
            p.call_a(OP_ATTR_GET, ATTR_SIGINT)
            p.tax()
            with p.branch("beq"):
                p.store(API_A, EXIT_SIGINT)
                p.store(API_X, 0)
                p.call(OP_EXIT)
                p.stp()
            p.store(API_A, EXIT_CODE)
            p.store(API_X, 0)
            p.call(OP_EXIT)
            p.stp()
    p.jmp_abs("poll")
    return p


def test_tty_pipe():
    cmd = rp6502.Emu.command("--headless", "--phi2", "0", rom("tty_pipe"))
    r = subprocess.run(cmd, input=INPUT, capture_output=True, text=True,
                       timeout=60)
    for name, got, want in (("stdout", r.stdout, INPUT),
                            ("exit code", r.returncode, EXIT_CODE)):
        assert got == want, f"{name} {got!r}, expected {want!r}"


ROMS = {"tty_pipe": prog}
TESTS = {"tty_pipe": test_tty_pipe}
main(globals())
