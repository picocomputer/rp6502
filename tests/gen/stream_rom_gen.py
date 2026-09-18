#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import pathlib

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rp6502_script  # noqa: E402
from rp6502_asm import (API_A, OP_CLOSE, OP_OPEN, OP_READ_XSTACK, O_RDONLY,
                        XSTACK, Asm)
from rp6502_rom import image

NAME = "M.DAT"
CHUNK = 16
DONE = b"stream ok\r\n"

HANDLE = 0x0200


def prog():
    p = Asm()

    p.push_str(NAME)
    p.lda_imm(O_RDONLY)
    p.sta_abs(API_A)
    p.call(OP_OPEN)
    p.sta_abs(HANDLE)

    p.symbol("loop")
    p.push(0)
    p.push(CHUNK)
    p.lda_abs(HANDLE)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)

    p.tax()
    with p.branch("beq"):
        p.symbol("inner")
        p.lda_abs(XSTACK)
        p.putc_a()
        p.dex()
        p.bne("inner")
        p.jmp_abs("loop")

    p.lda_abs(HANDLE)
    p.sta_abs(API_A)
    p.call(OP_CLOSE)
    for c in DONE:
        p.lda_imm(c)
        p.putc_a()
    p.stp()
    return p


def drive(emu, rom):
    # The payload has no zero byte because `wait` searches the console
    # capture as a C string, and a zero byte would hide everything printed
    # after it.
    payload = bytes(1 + (i * 7 + 11) % 0xFF for i in range(CHUNK * 5))
    pathlib.Path(NAME).write_bytes(payload)

    def body(e):
        e.cmd(f'wait "{DONE.decode().rstrip()}"')
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
        print(f"stream.rp6502 {image(prog()).write(a.emit)} bytes")
    if a.drive:
        return drive(a.emu, a.rom)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
