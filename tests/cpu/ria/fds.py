#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import os
import subprocess

import rp6502
from rp6502_asm import (API_A, API_X, O_CREAT, O_RDWR, O_WRONLY, OP_CLOSE,
                        OP_DUP2, OP_EXEC, OP_EXIT, OP_ISATTY, OP_OPEN,
                        OP_WRITE_XSTACK, XSTACK, Asm)
from rp6502_rom import main, rom

WRONG = 0x10  # zero page byte that counts what came out wrong
FD = 0x11
CHILD = "fds_child.rp6502"
SAID = "child\n"


def wrong_unless(p, want):
    """A is the low byte of a result; anything but want is counted wrong."""
    p.cmp_imm(want)
    with p.branch("beq"):
        p.inc_abs(WRONG)


def exec_child(p):
    # The argv: its table, argv[0] at 4 and the end, then argv[0].
    p.push_str(CHILD)
    p.push(0)
    p.push(0)
    p.pushw(4)
    p.call(OP_EXEC)


def exit_with_wrong(p):
    p.lda_abs(WRONG)
    p.sta_abs(API_A)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()


def parent():
    """Checks that 1 is a terminal and that an open SAVE: file refuses an
    exec, then puts the console's stderr on 1 and execs the child, which
    writes to 1."""
    p = Asm()
    p.store(WRONG, 0)
    p.call_a(OP_ISATTY, 1)
    wrong_unless(p, 1)

    p.push_str("SAVE:held.dat")
    p.call_a(OP_OPEN, O_WRONLY | O_CREAT)
    p.sta_zp(FD)
    exec_child(p)
    wrong_unless(p, 0xFF)
    p.lda_zp(FD)
    p.sta_abs(API_A)
    p.call(OP_CLOSE)
    wrong_unless(p, 0)

    p.push_str("CON:2")
    p.call_a(OP_OPEN, O_RDWR)
    p.sta_zp(FD)
    p.push(0)
    p.lda_zp(FD)
    p.sta_abs(XSTACK)
    p.call_a(OP_DUP2, 1)
    wrong_unless(p, 1)
    p.lda_zp(FD)
    p.sta_abs(API_A)
    p.call(OP_CLOSE)
    p.call_a(OP_ISATTY, 1)
    wrong_unless(p, 1)

    p.lda_abs(WRONG)
    with p.branch("bne"):
        exec_child(p)
    p.inc_abs(WRONG)
    exit_with_wrong(p)
    return p


def child():
    """Writes to 1, which its parent left on the console's stderr."""
    p = Asm()
    p.store(WRONG, 0)
    for c in reversed(SAID.encode("latin-1")):
        p.push(c)
    p.call_a(OP_WRITE_XSTACK, 1)
    wrong_unless(p, len(SAID))
    exit_with_wrong(p)
    return p


def test_exec_passes_descriptors():
    """The descriptors pass through an exec: what the child writes to 1 is
    stderr, because the parent put CON:2 there."""
    cmd = rp6502.Emu.command("--headless", "--phi2", "0", rom("fds"))
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60,
                       cwd=os.environ["RP6502_ROMS"])
    for name, got, want in (("stdout", r.stdout, ""),
                            ("stderr", r.stderr, SAID),
                            ("exit code", r.returncode, 0)):
        assert got == want, f"{name} {got!r}, expected {want!r}"


ROMS = {"fds": parent, "fds_child": child}
TESTS = {"exec_passes_descriptors": test_exec_passes_descriptors}
main(globals())
