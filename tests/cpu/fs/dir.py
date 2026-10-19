#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import os

import rp6502
from rp6502_asm import (API_A, API_X, OP_CLOSEDIR, OP_DROP_XSTACK, OP_EXIT,
                        OP_GETFREE, OP_GETLABEL, OP_OPENDIR, OP_READDIR,
                        XSTACK, Asm, putc, puthex, puthex16,
                        putnib)
from rp6502_rom import main, rom, save_dir

SIZE_L = 0x0200
SIZE_H = 0x0201
ATTR = 0x0202
DES = 0x0203
TMP = 0x0204
RES = 0x0205


def drop(p, n):
    p.ldy_imm(n)
    loop = p.local("drop")
    p.symbol(loop)
    p.lda_abs(XSTACK)
    p.dey()
    p.bne(loop)


def result(p, name):
    p.say(name + " ")
    p.txa()
    p.jsr_abs("puthex")
    p.say("\r\n")


def prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex, puthex16(TMP))
    p.symbol("main")

    p.push_str("")
    p.call(OP_OPENDIR)
    p.sta_abs(DES)
    result(p, "opendir")

    p.symbol("next")
    # READDIR requires an empty XSTACK, so the unread part of the previous
    # entry is dropped first.
    p.call(OP_DROP_XSTACK)
    p.lda_abs(DES)
    p.sta_abs(API_A)
    p.call(OP_READDIR)
    p.stx_abs(RES)
    p.txa()
    p.bne("end")
    # The fields are popped in f_stat_t order: a four byte fsize, eight bytes
    # of dates and times, fattrib, a 13 byte altname, then fname.
    p.lda_abs(XSTACK)
    p.sta_abs(SIZE_L)
    p.lda_abs(XSTACK)
    p.sta_abs(SIZE_H)
    drop(p, 2 + 8)
    p.lda_abs(XSTACK)
    p.sta_abs(ATTR)
    drop(p, 13)
    p.lda_abs(XSTACK)
    p.beq("end")
    p.symbol("name")
    p.jsr_abs("putc")
    p.lda_abs(XSTACK)
    p.bne("name")
    p.say(" ")
    p.lda_abs(ATTR)
    p.jsr_abs("puthex")
    p.say(" ")
    p.lda_abs(SIZE_H)
    p.ldx_abs(SIZE_L)
    p.jsr_abs("puthex16")
    p.say("\r\n")
    p.jmp_abs("next")

    p.symbol("end")
    p.ldx_abs(RES)
    result(p, "readdir")

    p.lda_abs(DES)
    p.sta_abs(API_A)
    p.call(OP_CLOSEDIR)
    result(p, "closedir")

    p.push_str("")
    p.call(OP_GETLABEL)
    result(p, "getlabel")

    p.push_str("")
    p.call(OP_GETFREE)
    result(p, "getfree")

    p.store(API_A, 0)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()
    return p


def test_dir():
    """Lists a folder of two files and a subfolder. The listing comes in the
    host's order, which is the order os.scandir gives for the same folder, so
    the entries are expected in that order."""
    here = save_dir() / "listed"
    here.mkdir(parents=True, exist_ok=True)
    (here / "alpha.txt").write_bytes(b"hello")
    (here / "beta.dat").write_bytes(b"wider content here")
    (here / "subdir").mkdir(exist_ok=True)
    os.chdir(here)

    def entry(found):
        if found.is_dir():
            return f"{found.name} 10 0000"
        return f"{found.name} 20 {found.stat().st_size:04X}"

    with rp6502.Emu(rom("dir")) as e:
        e.cmd("wait exit 20")
        e.cmd("expect exit 0")
        e.cmd('expect "opendir 00\\r\\n"')
        for found in os.scandir("."):
            e.cmd(f'expect "{entry(found)}\\r\\n"')
        e.cmd('expect "readdir 00\\r\\n"')
        e.cmd('expect "closedir 00\\r\\n"')
        # GETLABEL requires a drive name, so it returns -1 for "".
        e.cmd('expect "getlabel FF\\r\\n"')
        e.cmd('expect "getfree 00\\r\\n"')


ROMS = {"dir": prog}
TESTS = {"dir": test_dir}
main(globals())
