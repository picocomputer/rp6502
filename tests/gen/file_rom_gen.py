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

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rp6502_script  # noqa: E402
from rp6502_asm import (API_A, OP_CLOSE, OP_OPEN, OP_READ_XSTACK,
                        OP_WRITE_XSTACK, O_CREAT, O_RDONLY, O_TRUNC,
                        O_WRONLY, XSTACK, Asm)
from rp6502_rom import image

NAME = "T.DAT"
PAYLOAD = b"pocket file ok\r\n"



def prog():
    p = Asm()

    # Create it, write the payload, close.
    p.push_str(NAME)
    p.lda_imm(O_WRONLY | O_CREAT | O_TRUNC)
    p.sta_abs(API_A)
    p.call(OP_OPEN)
    p.sta_abs(0x0200)  # the descriptor

    for c in reversed(PAYLOAD):
        p.push(c)
    p.lda_abs(0x0200)
    p.sta_abs(API_A)
    p.call(OP_WRITE_XSTACK)

    p.lda_abs(0x0200)
    p.sta_abs(API_A)
    p.call(OP_CLOSE)

    # Open it again under the drive name the real machine uses — the
    # prefix has to reach the same file — read it back, print it.
    p.push_str("MSC0:" + NAME)
    p.lda_imm(O_RDONLY)
    p.sta_abs(API_A)
    p.call(OP_OPEN)
    p.sta_abs(0x0200)

    p.push(0)
    p.push(len(PAYLOAD))
    p.lda_abs(0x0200)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)

    p.tax()  # bytes read
    with p.branch("beq"):
        p.symbol("loop")
        p.lda_abs(XSTACK)
        p.putc_a()
        p.dex()
        p.bne("loop")

    p.lda_abs(0x0200)
    p.sta_abs(API_A)
    p.call(OP_CLOSE)
    p.stp()
    return p


def emit(path, body):
    return image(body).write(path)


def drive(emu, rom):
    """The other half of this file: the program above, watched. What it
    prints is PAYLOAD, so that is what this waits for -- one constant, one
    file, nothing to keep in step."""
    def body(e):
        e.cmd(f'wait "{PAYLOAD.decode().rstrip()}"')
    return rp6502_script.drive(emu, rom, body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    ap.add_argument("--drive", action="store_true",
                    help="run the ROM on the emulator and check what it says")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the .rp6502 --emit wrote")
    a = ap.parse_args()
    if a.emit:
        print(f"file.rp6502 {emit(a.emit, prog())} bytes")
    if a.drive:
        return drive(a.emu, a.rom)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
