#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import os
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rp6502_script  # noqa: E402
from rp6502_asm import OP_ARGV, XSTACK, Asm, putc, putnib, puthex
from rp6502_rom import image

BAR = ord("|")


def prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex)
    p.symbol("main")

    p.say("argv[")
    p.call(OP_ARGV)

    p.tax()
    p.beq("done")
    p.symbol("emit")
    p.lda_abs(XSTACK)
    p.bne("show")
    p.lda_imm(BAR)
    p.symbol("show")
    p.putc_a()
    p.dex()
    p.bne("emit")

    p.symbol("done")
    p.say("]\r\n")
    p.stp()
    return p


def drive(emu, rom, save_dir):
    """Empty arguments keep their places, which CMake cannot pass because it
    drops an empty string from a list. Then a copy of the ROM is loaded by a
    relative path, and its argv[0] is the absolute path with its drive."""
    here = Path(save_dir).resolve()
    shutil.copy(rom, here / "x.rp6502")
    os.chdir(here)
    cwd = here.as_posix() if os.name == "nt" else "FS:" + here.as_posix()

    def body(e):
        e.cmd('wait ".rp6502||Foo||]\\r\\n"')
        e.cmd("sys stop")
        e.cmd('load "x.rp6502"')
        e.cmd(f'wait "|||{cwd}/x.rp6502|]\\r\\n"')
    return rp6502_script.drive(emu, rom, body, save_dir=save_dir,
                               rom_args=("", "Foo", ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    ap.add_argument("--drive", action="store_true",
                    help="run the ROM on the emulator and check what it says")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the .rp6502 --emit wrote")
    ap.add_argument("--save-dir", help="the folder behind SAVE:")
    a = ap.parse_args()
    if a.emit:
        print(f"argv.rp6502 {image(prog()).write(a.emit)} bytes")
    if a.drive:
        return drive(a.emu, a.rom, a.save_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
