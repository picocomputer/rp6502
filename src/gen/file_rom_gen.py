#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# A .rp6502 that writes a file, closes it, opens it again, reads it
# back, and prints what came back. tests/host/pocket/test_pfile.cpp asserts
# the printed bytes, so the whole path — the 6502's syscalls, the
# shared std.c, the Pocket's MSC0: driver, pocket_file, and a host
# playing the APF target commands — is proven by one string arriving.
#
# It ships in the package too. A round trip is the one thing about a
# filesystem that cannot be checked by looking, and on hardware this
# prints its answer to a console the terminal is still showing.

import argparse

from rp6502_rom import (API_A, OP_CLOSE, OP_OPEN, OP_READ_XSTACK,
                        OP_WRITE_XSTACK, O_CREAT, O_RDONLY, O_TRUNC,
                        O_WRONLY, XSTACK, Asm, image)

NAME = "T.DAT"
PAYLOAD = b"pocket file ok\r\n"



def prog():
    p = Asm()

    # Create it, write the payload, close.
    p.push_str(NAME)
    p.lda(O_WRONLY | O_CREAT | O_TRUNC)
    p.sta(API_A)
    p.call(OP_OPEN)
    p.sta(0x0200)  # the descriptor

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
    p.sta(0x0200)

    p.push(0)
    p.push(len(PAYLOAD))
    p.lda_abs(0x0200)
    p.sta(API_A)
    p.call(OP_READ_XSTACK)

    p.emit(0xAA)  # tax — bytes read
    p.emit(0xF0, 0x00)  # beq done, patched once the loop is sized
    skip = len(p.b)
    loop = len(p.b)
    p.lda_abs(XSTACK)
    p.putc_a()
    p.dex()
    back = loop - (len(p.b) + 2)
    p.emit(0xD0, back)  # bne loop
    p.b[skip - 1] = len(p.b) - skip

    p.lda_abs(0x0200)
    p.sta(API_A)
    p.call(OP_CLOSE)
    p.stp()
    return p


def emit(path, body):
    return image(body).write(path)


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
