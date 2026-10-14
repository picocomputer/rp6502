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
from rp6502_asm import (API_A, API_ERRNO, API_X, OP_DROP_XSTACK,
                        OP_ERRNO_OPT, OP_EXEC, OP_EXIT, XSTACK, Asm, putc,
                        puthex, putnib)  # noqa: E402
from rp6502_rom import image  # noqa: E402

TMP = 0x0200

# llvm-mos numbers, which the ROM selects before its first call.
ERRNO_OPT_LLVM = 2


def prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex)

    # A call's result as X then A in hex, then errno.
    p.symbol("result")
    p.sta_abs(TMP)
    p.txa()
    p.jsr_abs("puthex")
    p.lda_abs(TMP)
    p.jsr_abs("puthex")
    p.say(" ")
    p.lda_abs(API_ERRNO)
    p.jsr_abs("puthex")
    p.say("\r\n")
    p.rts()

    p.symbol("main")
    p.call_a(OP_ERRNO_OPT, ERRNO_OPT_LLVM)

    # argv[0] of 256 bytes is one more than a path may hold, so EXEC fails
    # and the program carries on.
    p.say("exec long ")
    p.push(0)
    p.ldx_imm(0)
    p.lda_imm(ord("y"))
    p.symbol("long")
    p.sta_abs(XSTACK)
    p.dex()
    p.bne("long")
    p.pushw(0)
    p.pushw(4)
    p.call(OP_EXEC)
    p.jsr_abs("result")
    p.call(OP_DROP_XSTACK)

    # A name relative to the working directory starts y.rp6502. If EXEC comes
    # back, it failed.
    p.push_str("y.rp6502")
    p.pushw(0)
    p.pushw(4)
    p.call(OP_EXEC)
    p.say("exec y.rp6502 ")
    p.lda_abs(API_A)
    p.jsr_abs("result")
    p.store(API_A, 1)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()
    return p


def drive(emu, rom, save_dir):
    """y.rp6502 is a copy of argv.rp6502, which prints its argv table. The
    first byte is argv[0]'s offset, 4, and argv[0] comes back absolute."""
    here = Path(save_dir).resolve() / "exec"
    here.mkdir(exist_ok=True)
    shutil.copy(Path(rom).parent / "argv.rp6502", here / "y.rp6502")
    os.chdir(here)
    cwd = here.as_posix() if os.name == "nt" else "FS:" + here.as_posix()

    def body(e):
        # 16 is EINVAL.
        e.cmd('wait "exec long FFFF 16\\r\\n"')
        e.cmd(f'wait "argv[\\x04|||{cwd}/y.rp6502|]\\r\\n"')
    return rp6502_script.drive(emu, rom, body, save_dir=save_dir)


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
        print(f"execrel.rp6502 {image(prog()).write(a.emit)} bytes")
    if a.drive:
        return drive(a.emu, a.rom, a.save_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
