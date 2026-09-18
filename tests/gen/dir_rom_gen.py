#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import (API_A, API_X, OP_CLOSEDIR, OP_EXIT, OP_GETFREE,
                        OP_GETLABEL, OP_OPENDIR, OP_READDIR, OP_ZXSTACK,
                        XSTACK, Asm, putc, puthex, puthex16,
                        putnib)  # noqa: E402
from rp6502_rom import image  # noqa: E402

SIZE_L = 0x0200
SIZE_H = 0x0201
ATTR = 0x0202
DES = 0x0203
TMP = 0x0204
RES = 0x0205


def drop(p, n):
    p.ldy_imm(n)
    loop = p.local("drop")
    p.symbol(loop)
    p.lda_abs(XSTACK)
    p.dey()
    p.bne(loop)


def result(p, name):
    p.say(name + " ")
    p.txa()
    p.jsr_abs("puthex")
    p.say("\r\n")


def prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex, puthex16(TMP))
    p.symbol("main")

    p.push_str("")
    p.call(OP_OPENDIR)
    p.sta_abs(DES)
    result(p, "opendir")

    p.symbol("next")
    # readdir pushes its 282 bytes onto whatever the 512 byte xstack still
    # holds, so the unread part of the previous entry is cleared first.
    p.call(OP_ZXSTACK)
    p.lda_abs(DES)
    p.sta_abs(API_A)
    p.call(OP_READDIR)
    p.stx_abs(RES)
    p.txa()
    p.bne("end")
    # The fields are popped in f_stat_t order: a four byte fsize, eight bytes
    # of dates and times, fattrib, a 13 byte altname, then fname.
    p.lda_abs(XSTACK)
    p.sta_abs(SIZE_L)
    p.lda_abs(XSTACK)
    p.sta_abs(SIZE_H)
    drop(p, 2 + 8)
    p.lda_abs(XSTACK)
    p.sta_abs(ATTR)
    drop(p, 13)
    p.lda_abs(XSTACK)
    p.beq("end")
    p.symbol("name")
    p.jsr_abs("putc")
    p.lda_abs(XSTACK)
    p.bne("name")
    p.say(" ")
    p.lda_abs(ATTR)
    p.jsr_abs("puthex")
    p.say(" ")
    p.lda_abs(SIZE_H)
    p.ldx_abs(SIZE_L)
    p.jsr_abs("puthex16")
    p.say("\r\n")
    p.jmp_abs("next")

    p.symbol("end")
    p.ldx_abs(RES)
    result(p, "readdir")

    p.lda_abs(DES)
    p.sta_abs(API_A)
    p.call(OP_CLOSEDIR)
    result(p, "closedir")

    p.push_str("")
    p.call(OP_GETLABEL)
    result(p, "getlabel")

    p.push_str("")
    p.call(OP_GETFREE)
    result(p, "getfree")

    p.store(API_A, 0)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    a = ap.parse_args()
    if a.emit:
        print(f"dir.rp6502 {image(prog()).write(a.emit)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
