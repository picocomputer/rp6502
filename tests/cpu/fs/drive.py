#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import os
import shutil
from pathlib import Path

import rp6502
from rp6502_asm import (API_A, API_ERRNO, API_X, OP_CHDIR, OP_CHDRIVE,
                        OP_CLOSE, OP_DROP_XSTACK, OP_ERRNO_OPT, OP_EXIT,
                        OP_GETCWD, OP_MKDIR, OP_OPEN, OP_OPENDIR,
                        OP_READ_XSTACK, OP_STAT, OP_REMOVE, O_CREAT, O_RDONLY,
                        O_RDWR, O_TRUNC, O_WRONLY, XSTACK, Asm, putc, puthex,
                        putnib)
from rp6502_rom import ADVENTURE, main, rom, save_dir

TMP = 0x0200
FD = 0x0201
PFXN = 0x0202
CWD = 0x7000

# llvm-mos numbers, which the ROM selects before its first call.
ERRNO_OPT_LLVM = 2


def prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex)

    # A call's result as X then A in hex.
    p.symbol("ax")
    p.sta_abs(TMP)
    p.txa()
    p.jsr_abs("puthex")
    p.lda_abs(TMP)
    p.jmp_abs("puthex")

    # Prints the working directory and keeps a copy at CWD.
    p.symbol("getcwd")
    p.say("getcwd ")
    p.call(OP_GETCWD)
    p.ldx_imm(0)
    p.symbol("getcwd.top")
    p.lda_abs(XSTACK)
    p.sta_abx(CWD)
    p.beq("getcwd.end")
    p.jsr_abs("putc")
    p.inx()
    p.bne("getcwd.top")
    p.symbol("getcwd.end")
    p.call(OP_DROP_XSTACK)
    p.say("\r\n")
    p.rts()

    # Pushes the drive name, kept from the first getcwd, on top of a string
    # already pushed.
    p.symbol("push_pfx")
    p.ldx_abs(PFXN)
    p.symbol("push_pfx.top")
    p.lda_abx(CWD - 1)
    p.sta_abs(XSTACK)
    p.dex()
    p.bne("push_pfx.top")
    p.rts()

    p.symbol("main")

    def path_call(label, op, path, pfx=False):
        p.say(f"{label} ")
        p.push_str(path)
        if pfx:
            p.jsr_abs("push_pfx")
        p.call(op)
        p.jsr_abs("ax")
        p.say("\r\n")

    def stat(label, path, pfx=False):
        path_call(label, OP_STAT, path, pfx)
        p.call(OP_DROP_XSTACK)

    def open_fails(label, path, flags):
        p.say(f"open {label} ")
        p.push_str(path)
        p.call_a(OP_OPEN, flags)
        p.jsr_abs("ax")
        p.say(" ")
        p.lda_abs(API_ERRNO)
        p.jsr_abs("puthex")
        p.say("\r\n")

    # Prints the first n bytes of the file.
    def read(path, n):
        p.say(f"read {path} ")
        p.push_str(path)
        p.call_a(OP_OPEN, O_RDONLY)
        p.sta_abs(FD)
        p.pushw(n)
        p.call(OP_READ_XSTACK)
        p.ldx_imm(n)
        top = p.local("read")
        p.symbol(top)
        p.lda_abs(XSTACK)
        p.jsr_abs("putc")
        p.dex()
        p.bne(top)
        p.lda_abs(FD)
        p.sta_abs(API_A)
        p.call(OP_CLOSE)
        p.say("\r\n")

    p.call_a(OP_ERRNO_OPT, ERRNO_OPT_LLVM)

    # Installed files open by name, in any case, from another folder.
    path_call("mkdir elsewhere", OP_MKDIR, "elsewhere")
    path_call("chdir elsewhere", OP_CHDIR, "elsewhere")
    read(":ADVENTURE.RP6502", 8)
    read(":second.rp6502", 12)

    # They open only for reading, and other names on the null drive do not
    # open at all.
    open_fails("rw :adventure.rp6502", ":adventure.rp6502", O_RDWR)
    open_fails(":nope.rp6502", ":nope.rp6502", O_RDONLY)
    open_fails(":", ":", O_RDONLY)

    # Without the colon, the name is the file in the working directory.
    path_call("chdir ..", OP_CHDIR, "..")
    read("adventure.rp6502", 8)

    p.jsr_abs("getcwd")
    # The drive name is the working directory up to and with its colon.
    p.ldx_imm(0)
    p.symbol("find_pfx")
    p.lda_abx(CWD)
    p.inx()
    p.cmp_imm(ord(":"))
    p.bne("find_pfx")
    p.stx_abs(PFXN)

    # The null drive has no folders, so these all fail.
    stat("stat :adventure.rp6502", ":adventure.rp6502")
    path_call("opendir :", OP_OPENDIR, ":")
    path_call("chdir :adventure.rp6502", OP_CHDIR, ":adventure.rp6502")
    path_call("chdrive :", OP_CHDRIVE, ":")
    path_call("remove :adventure.rp6502", OP_REMOVE, ":adventure.rp6502")
    path_call("mkdir :sub", OP_MKDIR, ":sub")
    stat("stat drive::adventure.rp6502", ":adventure.rp6502", pfx=True)

    # A name with the drive in front is the file in the working directory,
    # and chdir can climb above the folder it started in.
    p.say("open drive:save.dat ")
    p.push_str("save.dat")
    p.jsr_abs("push_pfx")
    p.call_a(OP_OPEN, O_WRONLY | O_CREAT | O_TRUNC)
    p.sta_abs(API_A)
    p.txa()
    p.jsr_abs("puthex")
    p.call(OP_CLOSE)
    p.say("\r\n")
    path_call("mkdir sub", OP_MKDIR, "sub")
    path_call("chdir sub", OP_CHDIR, "sub")
    p.jsr_abs("getcwd")
    path_call("chdir ..", OP_CHDIR, "..")
    p.jsr_abs("getcwd")
    path_call("chdir ..", OP_CHDIR, "..")
    p.jsr_abs("getcwd")

    p.store(API_A, 0)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()
    return p


def test_drive():
    """Runs in a fresh folder holding second.rp6502 and a decoy
    adventure.rp6502. The script installs second.rp6502 and the real
    adventure ROM, so only a name without the colon reaches the decoy."""
    here = save_dir() / "drive"
    shutil.rmtree(here, ignore_errors=True)
    here.mkdir(parents=True)
    (here / "adventure.rp6502").write_bytes(b"NOT THE ROM")
    (here / "second.rp6502").write_bytes(b"#!RP6502 two")
    os.chdir(here)
    cwd = here.as_posix() if os.name == "nt" else "FS:" + here.as_posix()
    parent = (here.parent.as_posix() if os.name == "nt"
              else "FS:" + here.parent.as_posix())

    with rp6502.Emu(rom("drive")) as e:
        e.cmd(f'install "{Path(ADVENTURE).as_posix()}"')
        e.cmd('install "second.rp6502"')
        e.cmd("wait exit 60")
        e.cmd("expect exit 0")
        e.cmd('expect "mkdir elsewhere 0000\\r\\n"')
        e.cmd('expect "chdir elsewhere 0000\\r\\n"')
        e.cmd('expect "read :ADVENTURE.RP6502 #!RP6502\\r\\n"')
        e.cmd('expect "read :second.rp6502 #!RP6502 two\\r\\n"')
        # 13 is ENODEV.
        e.cmd('expect "open rw :adventure.rp6502 FFFF 13\\r\\n"')
        e.cmd('expect "open :nope.rp6502 FFFF 13\\r\\n"')
        e.cmd('expect "open : FFFF 13\\r\\n"')
        e.cmd('expect "chdir .. 0000\\r\\n"')
        e.cmd('expect "read adventure.rp6502 NOT THE \\r\\n"')
        e.cmd(f'expect "getcwd {cwd}\\r\\n"')
        e.cmd('expect "stat :adventure.rp6502 FFFF\\r\\n"')
        e.cmd('expect "opendir : FFFF\\r\\n"')
        e.cmd('expect "chdir :adventure.rp6502 FFFF\\r\\n"')
        e.cmd('expect "chdrive : FFFF\\r\\n"')
        e.cmd('expect "remove :adventure.rp6502 FFFF\\r\\n"')
        e.cmd('expect "mkdir :sub FFFF\\r\\n"')
        e.cmd('expect "stat drive::adventure.rp6502 FFFF\\r\\n"')
        e.cmd('expect "open drive:save.dat 00\\r\\n"')
        assert (here / "save.dat").exists(), f"no save.dat in {here}"
        e.cmd('expect "mkdir sub 0000\\r\\n"')
        e.cmd('expect "chdir sub 0000\\r\\n"')
        e.cmd(f'expect "getcwd {cwd}/sub\\r\\n"')
        e.cmd('expect "chdir .. 0000\\r\\n"')
        e.cmd(f'expect "getcwd {cwd}\\r\\n"')
        e.cmd('expect "chdir .. 0000\\r\\n"')
        e.cmd(f'expect "getcwd {parent}\\r\\n"')
        # A load by the installed name boots it from another folder.
        e.cmd('load ":adventure.rp6502"')
        e.cmd('wait "Colossal Cave Adventure"')


ROMS = {"drive": prog}
TESTS = {"drive": test_drive}
main(globals())
