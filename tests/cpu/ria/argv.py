#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import os
import shutil

import rp6502
from rp6502_asm import OP_ARGV, OP_EXEC, XSTACK, Asm, putc, putnib, puthex
from rp6502_rom import main, rom, save_dir, script

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


def prog_long():
    """EXEC with a path of 256 bytes, one more than a path holds, and then
    ARGV. The program prints the low byte that EXEC returns and the size
    that ARGV returns."""
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex)
    p.symbol("main")

    p.push(0)
    p.ldx_imm(0)
    p.symbol("fill")
    p.lda_imm(ord("A"))
    p.sta_abs(XSTACK)
    p.dex()
    p.bne("fill")
    for b in (0, 0, 0, 4):
        p.push(b)
    p.call(OP_EXEC)
    p.jsr_abs("puthex")

    p.call(OP_ARGV)
    p.pha()
    p.txa()
    p.jsr_abs("puthex")
    p.pla()
    p.jsr_abs("puthex")
    p.say("\r\n")
    p.stp()
    return p


ARGS_SCRIPT = r"""
# The program prints its argv with a | for each zero byte, so the argument
# given after the ROM appears between two of them.

wait ".rp6502|Foo|]\r\n"
"""


def test_args():
    script(ARGS_SCRIPT, rom("argv"), "--", "Foo")


def test_argv():
    """Empty arguments keep their places, which CMake cannot pass because it
    drops an empty string from a list. Then a copy of the ROM is loaded by a
    relative path, and its argv[0] is the absolute path with its drive."""
    here = save_dir()
    here.mkdir(exist_ok=True)
    shutil.copy(rom("argv"), here / "x.rp6502")
    os.chdir(here)
    cwd = here.as_posix() if os.name == "nt" else "FS:" + here.as_posix()
    with rp6502.Emu(rom("argv"), "--", "", "Foo", "") as e:
        e.cmd('wait ".rp6502||Foo||]\\r\\n"')
        e.cmd("sys stop")
        e.cmd('load "x.rp6502"')
        e.cmd(f'wait "|||{cwd}/x.rp6502|]\\r\\n"')


def test_exec_long():
    """A refused EXEC leaves the argv empty, so ARGV returns the 2 bytes of
    an empty table."""
    script('wait "FF0002\\r\\n"\n', rom("exec_long"))


ROMS = {"argv": prog, "exec_long": prog_long}
TESTS = {"args": test_args, "argv": test_argv, "exec_long": test_exec_long}
main(globals())
