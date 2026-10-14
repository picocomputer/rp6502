#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import (API_A, API_X, OP_ARGV, OP_ATTR_GET, OP_ATTR_SET,
                        OP_DROP_XSTACK, OP_EXEC, OP_EXIT, XSTACK, Asm, putc,
                        putnib, puthex)  # noqa: E402
from rp6502_rom import image  # noqa: E402

ATTR_LAUNCHER = 0x06
ATTR_EXIT_CODE = 0x07
CHILD = ":child.rp6502"
CHILD_EXIT = 7


def exit_with(p, code):
    p.store(API_A, code)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()


def launcher_prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex)
    p.symbol("main")

    # The launcher is never given arguments, so ARGV returns a table of four
    # bytes and then argv[0] alone.
    p.say("launcher ")
    p.call(OP_ARGV)
    p.ldx_imm(4)
    p.symbol("table")
    p.lda_abs(XSTACK)
    p.dex()
    p.bne("table")
    p.symbol("argv0")
    p.lda_abs(XSTACK)
    with p.branch("beq"):
        p.jsr_abs("putc")
        p.bra("argv0")
    p.say("\n")

    p.call_a(OP_ATTR_GET, ATTR_LAUNCHER)
    p.tax()
    p.beq("first")
    p.say("back ")
    p.call_a(OP_ATTR_GET, ATTR_EXIT_CODE)
    p.jsr_abs("puthex")
    p.say("\n")
    exit_with(p, 0)

    p.symbol("first")
    p.pushl(1)
    p.call_a(OP_ATTR_SET, ATTR_LAUNCHER)
    # The child's argv is CHILD and "exec": two offsets and a zero, then the
    # strings.
    p.push_str("exec")
    p.push_str(CHILD)
    p.pushw(0)
    p.pushw(6 + len(CHILD) + 1)
    p.pushw(6)
    p.call(OP_EXEC)
    exit_with(p, 1)
    return p


def child_prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc)
    p.symbol("main")

    # The second offset in ARGV's table is 0 when there is no argv[1].
    p.call(OP_ARGV)
    p.lda_abs(XSTACK)
    p.lda_abs(XSTACK)
    p.lda_abs(XSTACK)
    p.ora_abs(XSTACK)
    p.bne("again")
    p.say("child\n")
    exit_with(p, CHILD_EXIT)

    p.symbol("again")
    p.call(OP_DROP_XSTACK)
    p.say("child exec\n")
    p.push_str(CHILD)
    p.pushw(0)
    p.pushw(4)
    p.call(OP_EXEC)
    exit_with(p, 1)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit-launcher")
    ap.add_argument("--emit-child")
    a = ap.parse_args()
    if a.emit_launcher:
        n = image(launcher_prog()).write(a.emit_launcher)
        print(f"launcher.rp6502 {n} bytes")
    if a.emit_child:
        print(f"child.rp6502 {image(child_prog()).write(a.emit_child)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
