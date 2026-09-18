#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse

from rp6502_asm import (API_A, OP_CLOSE, OP_OPEN, OP_READ_XSTACK,
                        OP_WRITE_XSTACK, O_CREAT, O_RDONLY, O_TRUNC, O_WRONLY,
                        XSTACK, Asm, putc, putnib, puthex, puthex16)
from rp6502_rom import image

NAME = "T2.DAT"
# OP_READ_XSTACK returns the count's low byte in A and its high byte in X.
# A chunk under 256 bytes keeps the whole count in A, so the check loop
# runs once for each byte that was read.
CHUNK = 128
CHUNKS = 24
TOTAL = CHUNK * CHUNKS

FD = 0x0200
VAL = 0x0201
IDX = 0x0202
CHN = 0x0203
BADL, BADH = 0x0204, 0x0205
ERRL, ERRH = 0x0206, 0x0207
GOTL, GOTH = 0x0208, 0x0209
TMP = 0x020A
WROL, WROH = 0x020B, 0x020C
F1, F2 = 0x020D, 0x020E
RERR = 0x020F


def val(chunk, i):
    return (chunk * 37 + i * 7) & 0xFF



def build():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex, puthex16(TMP))

    p.symbol("main")

    for a, v in ((BADL, 0), (BADH, 0), (ERRL, 0xFF), (ERRH, 0xFF),
                 (GOTL, 0), (GOTH, 0), (WROL, 0), (WROH, 0),
                 (F1, 0xFF), (F2, 0xFF), (RERR, 0)):
        p.store(a, v)

    p.push_str(NAME)
    p.store(API_A, O_WRONLY | O_CREAT | O_TRUNC)
    p.call(OP_OPEN)
    p.sta_abs(FD)
    p.sta_abs(F1)

    for c in range(CHUNKS):
        p.store(VAL, val(c, CHUNK - 1))
        p.ldx_imm(CHUNK)
        top = p.symbol(p.local("push"))
        p.lda_abs(VAL)
        p.sta_abs(XSTACK)
        p.sec()
        p.sbc_imm(0x07)
        p.sta_abs(VAL)
        p.dex()
        p.bne(top)
        p.lda_abs(FD)
        p.sta_abs(API_A)
        p.call(OP_WRITE_XSTACK)
        p.cpx_imm(0x00)
        with p.branch("bne"):
            p.clc()
            p.adc_abs(WROL)
            p.sta_abs(WROL)
            with p.branch("bcc"):
                p.inc_abs(WROH)

    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_CLOSE)

    p.push_str(NAME)
    p.store(API_A, O_RDONLY)
    p.call(OP_OPEN)
    p.sta_abs(FD)
    p.sta_abs(F2)

    for c in range(CHUNKS):
        p.push(CHUNK >> 8)
        p.push(CHUNK & 0xFF)
        p.lda_abs(FD)
        p.sta_abs(API_A)
        p.call(OP_READ_XSTACK)
        p.cpx_imm(0x00)
        skip = p.local("skip")
        with p.branch("beq"):
            p.inc_abs(RERR)
            p.bne(skip)
        p.tax()
        with p.branch("beq"):
            p.store(VAL, val(c, 0))
            p.store(IDX, 0)
            p.store(CHN, c)

            top = p.symbol(p.local("check"))
            p.lda_abs(XSTACK)
            p.cmp_abs(VAL)
            with p.branch("beq"):
                p.inc16(BADL, BADH)
                p.lda_abs(ERRH)
                p.cmp_imm(0xFF)
                with p.branch("bne"):
                    p.lda_abs(CHN)
                    p.sta_abs(ERRH)
                    p.lda_abs(IDX)
                    p.sta_abs(ERRL)

            p.lda_abs(VAL)
            p.clc()
            p.adc_imm(0x07)
            p.sta_abs(VAL)
            p.inc_abs(IDX)
            p.inc16(GOTL, GOTH)
            p.dex()
            p.bne(top)
        p.symbol(skip)

    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_CLOSE)

    p.say("\r\nBIGFILE\r\nF=")
    p.lda_abs(F1)
    p.jsr_abs("puthex")
    p.lda_imm(ord("/"))
    p.jsr_abs("putc")
    p.lda_abs(F2)
    p.jsr_abs("puthex")
    p.say(" X=")
    p.lda_abs(RERR)
    p.jsr_abs("puthex")
    p.say("\r\nW=")
    p.lda_abs(WROH)
    p.ldx_abs(WROL)
    p.jsr_abs("puthex16")
    p.say(" R=")
    p.lda_abs(GOTH)
    p.ldx_abs(GOTL)
    p.jsr_abs("puthex16")
    p.say("\r\nE=")
    p.lda_abs(ERRH)
    p.ldx_abs(ERRL)
    p.jsr_abs("puthex16")
    p.say(" N=")
    p.lda_abs(BADH)
    p.ldx_abs(BADL)
    p.jsr_abs("puthex16")
    p.say("\r\n")

    p.lda_abs(BADL)
    p.ora_abs(BADH)
    p.ora_abs(RERR)
    p.bne("fail")
    p.lda_abs(GOTL)
    p.cmp_imm(TOTAL & 0xFF)
    p.bne("fail")
    p.lda_abs(GOTH)
    p.cmp_imm(TOTAL >> 8)
    p.bne("fail")
    p.say("PASS\r\n")
    p.jmp_abs("end")
    p.symbol("fail")
    p.say("FAIL\r\n")
    p.symbol("end")
    p.stp()
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    a = ap.parse_args()
    if a.emit:
        n = image(build()).write(a.emit)
        print(f"bigfile.rp6502 {n} bytes, {TOTAL} byte payload")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
