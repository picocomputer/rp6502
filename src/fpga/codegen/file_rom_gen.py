#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# A .rp6502 that writes a file, closes it, opens it again, reads it
# back, and prints what came back. tests/fpga/test_pfile.cpp asserts
# the printed bytes, so the whole path — the 6502's syscalls, the
# shared std.c, the Pocket's MSC0: driver, pocket_file, and a host
# playing the APF target commands — is proven by one string arriving.
#
# It ships in the package too. A round trip is the one thing about a
# filesystem that cannot be checked by looking, and on hardware this
# prints its answer to a console the terminal is still showing.

import argparse
import zlib
from pathlib import Path

ORG = 0x0300

XSTACK = 0xFFEC
API_OP = 0xFFEF
API_CALL = 0xFFF1
API_A = 0xFFF4
API_ERRNO = 0xFFED
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

NAME = "T.DAT"
PAYLOAD = b"pocket file ok\r\n"


class Prog:
    def __init__(self):
        self.b = bytearray()

    def lda(self, v):
        self.b += bytes((0xA9, v))

    def sta(self, a):
        self.b += bytes((0x8D, a & 0xFF, a >> 8))

    def lda_abs(self, a):
        self.b += bytes((0xAD, a & 0xFF, a >> 8))

    def push(self, v):
        self.lda(v)
        self.sta(XSTACK)

    def call(self, op):
        self.lda(op)
        self.sta(API_OP)
        self.b += bytes((0x20, API_CALL & 0xFF, API_CALL >> 8))

    def push_str(self, s):
        """Deepest byte first, so the string reads forward from the
        stack pointer the way std_api_open takes it."""
        for c in reversed(s.encode() + b"\0"):
            self.push(c)

    def putc_a(self):
        """Print the accumulator, waiting for the TX ready bit."""
        self.b += bytes((0x48,))  # pha
        self.b += bytes((0x2C, RIA_READY & 0xFF, RIA_READY >> 8))  # bit
        self.b += bytes((0x10, 0xFB))  # bpl -5
        self.b += bytes((0x68,))  # pla
        self.sta(RIA_TX)

    def spin(self):
        self.b += bytes((0xDB,))  # stp


def prog():
    p = Prog()

    # Create it, write the payload, close.
    p.push_str(NAME)
    p.lda(O_WRONLY | O_CREAT | O_TRUNC)
    p.sta(API_A)
    p.call(OP_OPEN)
    p.b += bytes((0x8D, 0x00, 0x02))  # sta $0200 — the descriptor

    for c in reversed(PAYLOAD):
        p.push(c)
    p.lda_abs(0x0200)
    p.sta(API_A)
    p.call(OP_WRITE_XSTACK)

    p.lda_abs(0x0200)
    p.sta(API_A)
    p.call(OP_CLOSE)

    # Open it again under the drive name the real machine uses — the
    # prefix has to reach the same file — read it back, print it.
    p.push_str("MSC0:" + NAME)
    p.lda(O_RDONLY)
    p.sta(API_A)
    p.call(OP_OPEN)
    p.b += bytes((0x8D, 0x00, 0x02))

    p.push(0)
    p.push(len(PAYLOAD))
    p.lda_abs(0x0200)
    p.sta(API_A)
    p.call(OP_READ_XSTACK)

    p.b += bytes((0xAA,))  # tax — bytes read
    p.b += bytes((0xF0, 0x00))  # beq done, patched once the loop is sized
    skip = len(p.b)
    loop = len(p.b)
    p.lda_abs(XSTACK)
    p.putc_a()
    p.b += bytes((0xCA,))  # dex
    back = loop - (len(p.b) + 2)
    p.b += bytes((0xD0, back & 0xFF))  # bne loop
    p.b[skip - 1] = len(p.b) - skip

    p.lda_abs(0x0200)
    p.sta(API_A)
    p.call(OP_CLOSE)
    p.spin()
    return p.b


def record(addr, data):
    head = f"${addr:05X} ${len(data):X} ${zlib.crc32(data) & 0xFFFFFFFF:08X}\n"
    return head.encode() + data


def emit(path, body):
    rom = b"#!RP6502\n"
    rom += record(ORG, body)
    rom += record(0xFFFC, bytes((ORG & 0xFF, ORG >> 8)))
    Path(path).write_bytes(rom)
    return len(rom)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    ap.add_argument("--print-payload", action="store_true")
    a = ap.parse_args()
    if a.print_payload:
        print(PAYLOAD.decode(), end="")
    if a.emit:
        print(f"file.rp6502 {emit(a.emit, prog())} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
