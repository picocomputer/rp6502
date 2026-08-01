#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# A .rp6502 that writes past the transfer window, so the parts of the
# file path file.rp6502 never reaches get exercised: msc.c's STD_PENDING
# return, the second chunk's offset advance, and the growth re-open at a
# nonzero offset.
#
# It exists for one question with no loud failure mode. Growing a file
# re-opens its slot with MSC_DS_RESIZE, and whether the Pocket preserves
# what was already there is undocumented — the flag is described, its
# effect on existing bytes is not. If a resize zeroes, everything but
# the last chunk comes back empty and nothing reports an error.
# Simulation cannot answer it: the bench models the behaviour we assumed.
#
# So the payload is computed, never stored — byte i of chunk c is
# (c * 37 + i * 7) & 0xFF, which repeats in neither — and the reader
# checks all of it. The offset of the first wrong byte is the diagnosis:
#
#   E=FFFF            nothing wrong
#   E=0000 N=0900     all but the last chunk gone: a resize zeroed the
#                     file, which is the thing being tested
#   E=0200 or a       a window boundary: a chunk landed at the wrong
#     multiple of it  offset rather than being lost
#
# A Pocket has no serial console, so the answer is four short lines a
# phone can read.

import argparse
import zlib
from pathlib import Path

ORG = 0x0300

XSTACK = 0xFFEC
API_OP = 0xFFEF
API_CALL = 0xFFF1
API_A = 0xFFF4
RIA_TX = 0xFFE1
RIA_READY = 0xFFE0

OP_OPEN = 0x14
OP_CLOSE = 0x15
OP_READ_XSTACK = 0x16
OP_WRITE_XSTACK = 0x18

O_RDONLY = 0x01
O_WRONLY = 0x02
O_CREAT = 0x10
O_TRUNC = 0x20

NAME = "T2.DAT"
# The read syscall answers with api_return_ax: A is the count's low byte
# and X its high. Keeping a chunk under 256 keeps the whole count in A,
# so the loop counter is the returned length and a short read shortens
# the loop instead of being mistaken for none at all.
CHUNK = 128
CHUNKS = 24  # 3072 bytes: six windows, so five boundary crossings
TOTAL = CHUNK * CHUNKS

FD = 0x0200
VAL = 0x0201   # running expected byte
IDX = 0x0202   # byte index within the chunk, 0..255
CHN = 0x0203   # chunk number, doubles as the offset's high byte
BADL, BADH = 0x0204, 0x0205  # wrong bytes seen
ERRL, ERRH = 0x0206, 0x0207  # offset of the first, FFFF for none
GOTL, GOTH = 0x0208, 0x0209  # bytes actually read back
TMP = 0x020A
WROL, WROH = 0x020B, 0x020C  # bytes the writes actually took
F1, F2 = 0x020D, 0x020E      # the two descriptors, FF when open failed
RERR = 0x020F                # a read answered -1


def val(chunk, i):
    return (chunk * 37 + i * 7) & 0xFF


class Prog:
    def __init__(self):
        self.b = bytearray()

    def emit(self, *by):
        self.b += bytes(by)

    def here(self):
        return ORG + len(self.b)

    def lda(self, v):
        self.emit(0xA9, v & 0xFF)

    def ldx(self, v):
        self.emit(0xA2, v & 0xFF)

    def sta(self, a):
        self.emit(0x8D, a & 0xFF, a >> 8)

    def lda_abs(self, a):
        self.emit(0xAD, a & 0xFF, a >> 8)

    def ldx_abs(self, a):
        self.emit(0xAE, a & 0xFF, a >> 8)

    def cmp_abs(self, a):
        self.emit(0xCD, a & 0xFF, a >> 8)

    def ora_abs(self, a):
        self.emit(0x0D, a & 0xFF, a >> 8)

    def inc_abs(self, a):
        self.emit(0xEE, a & 0xFF, a >> 8)

    def jsr(self, a):
        self.emit(0x20, a & 0xFF, a >> 8)

    def jmp(self, a):
        self.emit(0x4C, a & 0xFF, a >> 8)

    def rts(self):
        self.emit(0x60)

    def store(self, a, v):
        self.lda(v)
        self.sta(a)

    def push(self, v):
        self.store(XSTACK, v)

    def call(self, op):
        self.store(API_OP, op)
        self.jsr(API_CALL)

    def push_str(self, s):
        for c in reversed(s.encode() + b"\0"):
            self.push(c)

    def inc16(self, lo, hi):
        """++[hi:lo], carrying without disturbing A."""
        self.inc_abs(lo)
        self.emit(0xD0, 0x03)  # bne over
        self.inc_abs(hi)

    def branch(self, op):
        """Emit a forward branch and return a handle to close it."""
        self.emit(op, 0x00)
        return len(self.b)

    def close(self, h):
        self.b[h - 1] = len(self.b) - h


def build():
    p = Prog()
    p.emit(0x4C, 0x00, 0x00)  # jmp main
    jmp_main = 1

    # --- putc: A to the console, once it will take it ---
    putc = p.here()
    p.emit(0x48)  # pha
    p.emit(0x2C, RIA_READY & 0xFF, RIA_READY >> 8)  # bit
    p.emit(0x10, 0xFB)  # bpl -5
    p.emit(0x68)  # pla
    p.sta(RIA_TX)
    p.rts()

    # --- putnib: low nibble of A as one hex digit ---
    putnib = p.here()
    p.emit(0xC9, 0x0A)  # cmp #10
    p.emit(0xB0, 0x06)  # bcs letter (over clc/adc/jmp)
    p.emit(0x18)  # clc
    p.emit(0x69, ord("0"))
    p.jmp(putc)
    p.emit(0x18)  # letter: clc
    p.emit(0x69, ord("A") - 10)
    p.jmp(putc)

    # --- puthex: A as two hex digits ---
    puthex = p.here()
    p.emit(0x48)  # pha
    p.emit(0x4A, 0x4A, 0x4A, 0x4A)  # lsr x4
    p.jsr(putnib)
    p.emit(0x68)  # pla
    p.emit(0x29, 0x0F)
    p.jmp(putnib)

    # --- puthex16: A high, X low ---
    puthex16 = p.here()
    p.emit(0x8E, TMP & 0xFF, TMP >> 8)  # stx TMP
    p.jsr(puthex)
    p.lda_abs(TMP)
    p.jmp(puthex)

    def text(s):
        for c in s.encode():
            p.lda(c)
            p.jsr(putc)

    # --- main ---
    main = p.here()
    p.b[jmp_main] = main & 0xFF
    p.b[jmp_main + 1] = main >> 8

    for a, v in ((BADL, 0), (BADH, 0), (ERRL, 0xFF), (ERRH, 0xFF),
                 (GOTL, 0), (GOTH, 0), (WROL, 0), (WROH, 0),
                 (F1, 0xFF), (F2, 0xFF), (RERR, 0)):
        p.store(a, v)

    # Create, then write CHUNKS x CHUNK bytes.
    p.push_str(NAME)
    p.store(API_A, O_WRONLY | O_CREAT | O_TRUNC)
    p.call(OP_OPEN)
    p.sta(FD)
    p.sta(F1)

    for c in range(CHUNKS):
        # Push descending, so the write reads forward as byte 0..255.
        p.store(VAL, val(c, CHUNK - 1))
        p.ldx(CHUNK)
        top = p.here()
        p.lda_abs(VAL)
        p.sta(XSTACK)
        p.emit(0x38)  # sec
        p.emit(0xE9, 0x07)  # sbc #7
        p.sta(VAL)
        p.emit(0xCA)  # dex
        p.emit(0xD0, (top - (p.here() + 2)) & 0xFF)
        p.lda_abs(FD)
        p.sta(API_A)
        p.call(OP_WRITE_XSTACK)
        # A is the low byte of what it took; X the high, or FF on error.
        p.emit(0xE0, 0x00)  # cpx #0
        wbad = p.branch(0xD0)
        p.emit(0x18)  # clc
        p.emit(0x6D, WROL & 0xFF, WROL >> 8)  # adc WROL
        p.sta(WROL)
        p.emit(0x90, 0x03)  # bcc over
        p.inc_abs(WROH)
        p.close(wbad)

    p.lda_abs(FD)
    p.sta(API_A)
    p.call(OP_CLOSE)

    # Read it back through the drive prefix, checking every byte.
    p.push_str("MSC0:" + NAME)
    p.store(API_A, O_RDONLY)
    p.call(OP_OPEN)
    p.sta(FD)
    p.sta(F2)

    for c in range(CHUNKS):
        p.push(CHUNK >> 8)
        p.push(CHUNK & 0xFF)
        p.lda_abs(FD)
        p.sta(API_A)
        p.call(OP_READ_XSTACK)
        # X is the count's high byte, and FF when the call failed. Either
        # way a nonzero high byte means this is not a length we asked for.
        p.emit(0xE0, 0x00)  # cpx #0
        rok = p.branch(0xF0)
        p.inc_abs(RERR)
        rbad = p.branch(0xD0)  # bne: always, skip the chunk
        p.close(rok)
        p.emit(0xAA)  # tax — bytes returned
        short = p.branch(0xF0)  # beq: nothing came back, skip the chunk

        p.store(VAL, val(c, 0))
        p.store(IDX, 0)
        p.store(CHN, c)

        top = p.here()
        p.lda_abs(XSTACK)  # pops one byte
        p.cmp_abs(VAL)
        good = p.branch(0xF0)  # beq good
        p.inc16(BADL, BADH)
        p.lda_abs(ERRH)
        p.emit(0xC9, 0xFF)  # first one only
        have = p.branch(0xD0)  # bne have
        p.lda_abs(CHN)
        p.sta(ERRH)
        p.lda_abs(IDX)
        p.sta(ERRL)
        p.close(have)
        p.close(good)

        p.lda_abs(VAL)  # next expected
        p.emit(0x18)
        p.emit(0x69, 0x07)
        p.sta(VAL)
        p.inc_abs(IDX)
        p.inc16(GOTL, GOTH)
        p.emit(0xCA)  # dex
        p.emit(0xD0, (top - (p.here() + 2)) & 0xFF)
        p.close(short)
        p.close(rbad)

    p.lda_abs(FD)
    p.sta(API_A)
    p.call(OP_CLOSE)

    # --- the answer ---
    text("\r\nBIGFILE\r\nF=")
    p.lda_abs(F1)
    p.jsr(puthex)
    p.lda(ord("/"))
    p.jsr(putc)
    p.lda_abs(F2)
    p.jsr(puthex)
    text(" X=")
    p.lda_abs(RERR)
    p.jsr(puthex)
    text("\r\nW=")
    p.lda_abs(WROH)
    p.ldx_abs(WROL)
    p.jsr(puthex16)
    text(" R=")
    p.lda_abs(GOTH)
    p.ldx_abs(GOTL)
    p.jsr(puthex16)
    text("\r\nE=")
    p.lda_abs(ERRH)
    p.ldx_abs(ERRL)
    p.jsr(puthex16)
    text(" N=")
    p.lda_abs(BADH)
    p.ldx_abs(BADL)
    p.jsr(puthex16)
    text("\r\n")

    # PASS needs no wrong bytes and the whole file back.
    p.lda_abs(BADL)
    p.ora_abs(BADH)
    p.ora_abs(RERR)
    f1 = p.branch(0xD0)
    p.lda_abs(GOTL)
    p.emit(0xC9, TOTAL & 0xFF)
    f2 = p.branch(0xD0)
    p.lda_abs(GOTH)
    p.emit(0xC9, TOTAL >> 8)
    f3 = p.branch(0xD0)
    text("PASS\r\n")
    p.emit(0x4C, 0x00, 0x00)  # jmp end
    endj = len(p.b) - 2
    for h in (f1, f2, f3):
        p.close(h)
    text("FAIL\r\n")
    end = p.here()
    p.b[endj] = end & 0xFF
    p.b[endj + 1] = end >> 8
    p.emit(0xDB)  # stp
    return bytes(p.b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    a = ap.parse_args()
    if a.emit:
        body = build()
        rom = b"#!RP6502\n"
        for addr, data in ((ORG, body),
                           (0xFFFC, bytes((ORG & 0xFF, ORG >> 8)))):
            crc = zlib.crc32(data) & 0xFFFFFFFF
            rom += f"${addr:05X} ${len(data):X} ${crc:08X}\n".encode() + data
        Path(a.emit).write_bytes(rom)
        print(f"bigfile.rp6502 {len(rom)} bytes, {TOTAL} byte payload")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
