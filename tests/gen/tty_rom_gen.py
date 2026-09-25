#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rp6502_script  # noqa: E402
from rp6502_asm import (API_A, OP_CLOSE, OP_OPEN, OP_READ_XSTACK, O_RDONLY,
                        XSTACK, Asm, putc)  # noqa: E402
from rp6502_rom import image  # noqa: E402

FD = 0x0200

TYPED = "console reads"
END = "."


def prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc)

    p.symbol("main")
    p.push_str("TTY:")
    p.lda_imm(O_RDONLY)
    p.sta_abs(API_A)
    p.call(OP_OPEN)
    p.sta_abs(FD)

    p.symbol("poll")
    p.push(0)
    p.push(8)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)
    p.tax()
    with p.branch("beq"):
        p.symbol("echo")
        p.lda_abs(XSTACK)
        p.sta_abs(0x0201)
        p.jsr_abs("putc")
        p.lda_abs(0x0201)
        p.cmp_imm(ord(END))
        with p.branch("bne"):
            p.lda_abs(FD)
            p.sta_abs(API_A)
            p.call(OP_CLOSE)
            p.stp()
        p.dex()
        p.bne("echo")
    p.jmp_abs("poll")
    return p


def drive(emu, rom, save_dir=None):
    def body(e):
        e.cmd("run 10")
        e.cmd("key a+ctrl")
        e.cmd('wait "\\x01"')
        e.cmd('type "\\2"')
        e.cmd('wait "\\x02"')
        e.cmd('type "\\x41\\102"')
        e.cmd('wait "AB"')
        e.cmd("key space")
        e.cmd('wait " "')
        e.cmd("key minus")
        e.cmd('wait "-"')
        e.cmd("key 3+shift")
        e.cmd('wait "#"')
        e.cmd("key kp5")
        e.cmd('wait "5"')
        e.cmd("key a+shift")
        e.cmd('wait "A"')
        e.cmd(f'type "{TYPED}{END}"')
        e.cmd(f'wait "{TYPED}{END}"')
    return rp6502_script.drive(emu, rom, body, save_dir=save_dir)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--emit")
    ap.add_argument("--drive", action="store_true",
                    help="run the ROM on the emulator and type at it")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the .rp6502 --emit wrote")
    ap.add_argument("--save-dir", help="the folder behind SAVE:")
    a = ap.parse_args()
    if a.emit:
        print(f"tty.rp6502 {image(prog()).write(a.emit)} bytes")
    if a.drive:
        return drive(a.emu, a.rom, a.save_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
