#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse

from rp6502_asm import (API_A, OP_CLOSE, OP_LSEEK, OP_OPEN, O_CREAT,
                        O_RDONLY, O_TRUNC, O_WRONLY, Asm, putc, putnib,
                        puthex)
from rp6502_rom import image


SEEK_END_CC65 = 1

FD = 0x0200
OLD = "t2.bin"
NEW = "n2.bin"


def build():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex)
    p.symbol("main")

    text = p.say

    def open_name(name, flags):
        p.push_str(name)
        p.store(API_A, flags)
        p.call(OP_OPEN)
        p.sta_abs(FD)

    def close_fd():
        p.lda_abs(FD)
        p.cmp_imm(0xFF)
        with p.branch("beq"):
            p.lda_abs(FD)
            p.sta_abs(API_A)
            p.call(OP_CLOSE)

    def show_len(tag, name):
        open_name(name, O_RDONLY)
        text(tag + "=")
        p.lda_abs(FD)
        p.jsr_abs("puthex")
        text("/")
        for _ in range(4):
            p.push(0)
        p.push(SEEK_END_CC65)
        p.lda_abs(FD)
        p.sta_abs(API_A)
        p.call(OP_LSEEK)
        p.pha()
        p.txa()
        p.jsr_abs("puthex")
        p.pla()
        p.jsr_abs("puthex")
        text("\r\n")
        close_fd()

    def attempt(tag, name, flags):
        open_name(name, flags)
        text(tag + "=")
        p.lda_abs(FD)
        p.jsr_abs("puthex")
        text("\r\n")
        close_fd()

    show_len("L0", OLD)

    attempt("N", NEW, O_WRONLY | O_CREAT)
    show_len("L1", NEW)

    attempt("T", OLD, O_WRONLY | O_TRUNC)
    show_len("L2", OLD)

    text("DONE\r\n")
    p.stp()
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    a = ap.parse_args()
    if a.emit:
        n = image(build()).write(a.emit)
        print(f"probe.rp6502 {n} bytes, shrink probe on {OLD}, create {NEW}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
