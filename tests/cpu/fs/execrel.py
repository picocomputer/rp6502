#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import os
import shutil

import rp6502
from rp6502_asm import (API_A, API_ERRNO, API_X, OP_DROP_XSTACK,
                        OP_ERRNO_OPT, OP_EXEC, OP_EXIT, XSTACK, Asm, putc,
                        puthex, putnib)
from rp6502_rom import main, rom, save_dir

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


def test_execrel():
    """y.rp6502 is a copy of argv.rp6502, which prints its argv table. The
    first byte is argv[0]'s offset, 4, and argv[0] comes back absolute."""
    here = save_dir() / "exec"
    here.mkdir(parents=True, exist_ok=True)
    shutil.copy(rom("argv"), here / "y.rp6502")
    os.chdir(here)
    cwd = here.as_posix() if os.name == "nt" else "FS:" + here.as_posix()
    with rp6502.Emu(rom("execrel")) as e:
        # 16 is EINVAL.
        e.cmd('wait "exec long FFFF 16\\r\\n"')
        e.cmd(f'wait "argv[\\x04|||{cwd}/y.rp6502|]\\r\\n"')


ROMS = {"execrel": prog}
TESTS = {"execrel": test_execrel}
main(globals())
