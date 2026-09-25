#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The payload advances by 7 a byte and by a further 37 a chunk. With 7 alone
# the bytes would repeat every 256 bytes, so chunk n + 2 would equal chunk n
# and a read that returned an earlier chunk's bytes could pass. With the 37
# added, each chunk's first byte is 165 past the previous chunk's, and 165 is
# odd, so none of the 17 or fewer chunks in one sequence repeats another.

import argparse
import pathlib

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rp6502_script  # noqa: E402
from rp6502_asm import (API_A, API_ERRNO, OP_ARGV, OP_CHDIR, OP_CHDRIVE,
                        OP_CLOSE, OP_DROP_XSTACK, OP_ERRNO_OPT, OP_GETCWD,
                        OP_GMTIME, OP_LOCALTIME, OP_LSEEK, OP_OPEN,
                        OP_READ_XRAM, OP_READ_XSTACK, OP_RENAME, OP_STAT,
                        OP_SYNCFS, OP_TIME_GET, OP_WRITE_XRAM,
                        OP_WRITE_XSTACK, O_APPEND, O_CREAT, O_EXCL, O_RDONLY,
                        O_TRUNC, O_WRONLY, RW0_ADDR, RW0_DATA, SEEK_CUR,
                        SEEK_END, SEEK_SET, XSTACK, Asm, putc, puthex,
                        putnib)
from rp6502_rom import image


FD = 0x0200
VAL = 0x0201
TMP = 0x0202
PASSN = 0x0203
FAILN = 0x0204
TIDX = 0x0205
EXPL, EXPH = 0x0206, 0x0207
CNT = 0x0208
BAD = 0x0209
CNTL, CNTH = 0x020A, 0x020B
PFXN = 0x020C
TMP2 = 0x020D
ERRA, ERRB = 0x020E, 0x020F
FD2 = 0x0210
CWDN = 0x0211
CWDL, CWDH = 0x0212, 0x0213
ARGN = 0x0214
FDS = 0x0230
PFX = 0x0240
PFX_MAX = 8
CWD = 0x7000
ARGV0 = 0x7100
FAILS = 0x7200

CHUNK = 128
CHUNKS = 12
TOTAL = CHUNK * CHUNKS

# Every file the ROM creates is a SAVE: file, so the ROM writes nothing into
# the working directory, which on the Pocket is the folder of the core itself.
NAME = "SAVE:fs1.dat"
NAME2 = "SAVE:fs2.dat"
NAME3 = "SAVE:fs3.dat"
NAME4 = "SAVE:fs4.dat"
LONGEST = "SAVE:fs_32_chars_long_name.0123456789"

# Each of these breaks one of the SAVE: name rules.
REFUSED = ["SAVE:", "SAVE:..", "SAVE:../x", "SAVE:a/b", "SAVE:FS:/x",
           "SAVE:x.", "SAVE:CON", "SAVE:lpt1.sav", "SAVE:a b",
           "SAVE:" + "n" * 33]

GAP = 32

# llvm-mos numbers, which the ROM selects before its first call.
ERRNO_OPT_LLVM = 2
EACCES = 13
ENODEV = 19
EINVAL = 22
ENOSYS = 38

# The Pocket keeps its working directory in one fixed folder.
POCKET_CWD = "FS:/Assets/rp6502/common"

# The first XRAM read of NAME3 and the first of the asset each request one
# chunk more than the data holds, so a read that returns the requested length
# fails the check.
XCHUNKS = 5
XLEN = CHUNK * XCHUNKS
XWR = 0x1000
XRD = 0x4000
ASSET = "fstest.bin"
ACHUNKS = 17    # 2176 bytes, more than the 2048 std.c reads at a time
ALEN = CHUNK * ACHUNKS
XROM = 0x6000

# The first check fails if NAME exists and an exclusive create of NAME2 fails
# if NAME2 exists, so drive() deletes the files in CREATES from the SAVE:
# folder before each run.
CREATES = ([n[len("SAVE:"):] for n in (NAME, NAME2, NAME3, NAME4, LONGEST)]
           + ["pfx.dat"] + [f"s{i}.dat" for i in range(9)])

# The check that compares localtime with gmtime fails where the two agree, as
# they do on a host set to UTC, so the emulator is run in a time zone five
# hours behind UTC.
TZ = "EST5"


CHECKS = 0

PLATFORM = []


def payload(chunks):
    out = bytearray()
    val = 13
    for _ in range(chunks):
        for _ in range(CHUNK):
            out.append(val)
            val = (val + 7) & 0xFF
        val = (val + 37) & 0xFF
    return bytes(out)


def build():
    global CHECKS
    checks = [0]
    platform_only = []
    p = Asm()
    p.jmp_abs("main")

    p.use(putc, putnib, puthex)

    p.symbol("record")
    p.cmp_imm(0x00)
    with p.branch("bne"):
        p.lda_imm(ord("."))
        p.jsr_abs("putc")
        p.inc_abs(PASSN)
        p.bra("record.done")
    p.lda_imm(ord("X"))
    p.jsr_abs("putc")
    p.ldx_abs(FAILN)
    p.lda_abs(TIDX)
    p.sta_abx(FAILS)
    p.inc_abs(FAILN)
    p.symbol("record.done")
    p.inc_abs(TIDX)
    p.rts()

    p.symbol("is_ff")
    p.cmp_imm(0xFF)
    with p.branch("beq"):
        p.lda_imm(1)
        p.rts()
    p.lda_imm(0)
    p.rts()

    p.symbol("not_ff")
    p.cmp_imm(0xFF)
    with p.branch("beq"):
        p.lda_imm(0)
        p.rts()
    p.lda_imm(1)
    p.rts()

    p.symbol("is_zero")
    p.cmp_imm(0x00)
    with p.branch("beq"):
        p.lda_imm(1)
        p.rts()
    p.lda_imm(0)
    p.rts()

    p.symbol("eq16")
    p.cmp_abs(EXPL)
    p.bne("eq16.no")
    p.txa()
    p.cmp_abs(EXPH)
    p.bne("eq16.no")
    p.lda_imm(0)
    p.rts()
    p.symbol("eq16.no")
    p.lda_imm(1)
    p.rts()

    p.symbol("do_close")
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_CLOSE)
    p.rts()

    p.symbol("seek_end")
    for _ in range(4):
        p.push(0)
    p.push(SEEK_END)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_LSEEK)
    p.rts()

    p.symbol("wr_chunk")
    # EXPL and EXPH are stored before the call, because the call returns
    # the count in A and X and a store overwrites A.
    p.store(EXPL, CHUNK)
    p.store(EXPH, 0)
    p.lda_abs(VAL)
    p.clc()
    p.adc_imm((7 * (CHUNK - 1)) & 0xFF)
    p.sta_abs(TMP)
    p.ldx_imm(CHUNK)
    p.symbol("wr_chunk.top")
    p.lda_abs(TMP)
    p.sta_abs(XSTACK)
    p.sec()
    p.sbc_imm(0x07)
    p.sta_abs(TMP)
    p.dex()
    p.bne("wr_chunk.top")
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_WRITE_XSTACK)
    p.jsr_abs("eq16")
    p.pha()
    p.lda_abs(VAL)
    p.clc()
    p.adc_imm((7 * CHUNK + 37) & 0xFF)
    p.sta_abs(VAL)
    p.pla()
    p.rts()

    p.symbol("rd_chunk")
    p.store(BAD, 0)
    p.push(CHUNK >> 8)
    p.push(CHUNK & 0xFF)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)
    p.sta_abs(CNTL)
    p.stx_abs(CNTH)
    p.cpx_imm(0xFF)
    with p.branch("bne"):
        p.lda_imm(1)
        p.rts()
    # Every byte that arrived is popped whether it matched or not, because
    # the next call's arguments are pushed on top of anything left on the
    # xstack.
    p.ldx_abs(CNTL)
    with p.branch("beq"):
        p.symbol("rd_chunk.top")
        p.lda_abs(XSTACK)
        p.cmp_abs(VAL)
        with p.branch("beq"):
            p.inc_abs(BAD)
        p.lda_abs(VAL)
        p.clc()
        p.adc_imm(0x07)
        p.sta_abs(VAL)
        p.dex()
        p.bne("rd_chunk.top")
    p.lda_abs(VAL)
    p.clc()
    p.adc_imm(37)
    p.sta_abs(VAL)
    p.lda_abs(CNTL)
    p.cmp_imm(CHUNK)
    with p.branch("beq"):
        p.inc_abs(BAD)
    p.lda_abs(CNTH)
    with p.branch("beq"):
        p.inc_abs(BAD)
    p.lda_abs(BAD)
    p.rts()

    p.symbol("xfill")
    p.ldx_imm(CHUNK)
    p.symbol("xfill.top")
    p.lda_abs(VAL)
    p.sta_abs(RW0_DATA)
    p.clc()
    p.adc_imm(0x07)
    p.sta_abs(VAL)
    p.dex()
    p.bne("xfill.top")
    p.lda_abs(VAL)
    p.clc()
    p.adc_imm(37)
    p.sta_abs(VAL)
    p.dec_abs(CNT)
    p.bne("xfill")
    p.rts()

    p.symbol("xcheck")
    p.store(BAD, 0)
    p.symbol("xcheck.chunk")
    p.ldx_imm(CHUNK)
    p.symbol("xcheck.top")
    p.lda_abs(RW0_DATA)
    p.cmp_abs(VAL)
    # BAD is set rather than counted, because a one-byte count of 256 wrong
    # bytes would read as zero.
    with p.branch("beq"):
        p.store(BAD, 1)
    p.lda_abs(VAL)
    p.clc()
    p.adc_imm(0x07)
    p.sta_abs(VAL)
    p.dex()
    p.bne("xcheck.top")
    p.lda_abs(VAL)
    p.clc()
    p.adc_imm(37)
    p.sta_abs(VAL)
    p.dec_abs(CNT)
    p.bne("xcheck.chunk")
    p.lda_abs(BAD)
    p.rts()

    # The working directory goes to CWD and its drive name, up to and with
    # the colon, to PFX.
    p.symbol("cwd_buf")
    p.call(OP_GETCWD)
    p.sta_abs(CWDL)
    p.stx_abs(CWDH)
    p.ldx_imm(0)
    p.symbol("cwd_buf.top")
    p.lda_abs(XSTACK)
    p.sta_abx(CWD)
    p.beq("cwd_buf.end")
    p.inx()
    p.bne("cwd_buf.top")
    p.symbol("cwd_buf.end")
    p.stx_abs(CWDN)
    p.store(PFXN, 0)
    p.ldx_imm(0)
    p.symbol("cwd_buf.pfx")
    p.lda_abx(CWD)
    p.cmp_imm(ord(":"))
    with p.branch("bne"):
        p.sta_abx(PFX)
        p.inx()
        p.stx_abs(PFXN)
        p.rts()
    p.cmp_imm(ord("/"))
    with p.branch("bne"):
        p.rts()
    p.cmp_imm(ord("\\"))
    with p.branch("bne"):
        p.rts()
    p.cmp_imm(0)
    with p.branch("bne"):
        p.rts()
    p.sta_abx(PFX)
    p.inx()
    p.cpx_imm(PFX_MAX)
    with p.branch("bne"):
        p.rts()
    p.jmp_abs("cwd_buf.pfx")

    p.symbol("cwd_has_drive")
    p.lda_abs(PFXN)
    with p.branch("bne"):
        p.lda_imm(1)
        p.rts()
    p.ldx_abs(PFXN)
    p.lda_abx(CWD)
    p.cmp_imm(ord("/"))
    with p.branch("beq"):
        p.lda_imm(1)
        p.rts()
    p.lda_imm(0)
    p.rts()

    # The argv table ends at its zero pair, and argv[0] is the first string
    # after it.
    p.symbol("argv0_buf")
    p.call(OP_ARGV)
    p.symbol("argv0_buf.table")
    p.lda_abs(XSTACK)
    p.sta_abs(TMP)
    p.lda_abs(XSTACK)
    p.ora_abs(TMP)
    p.bne("argv0_buf.table")
    p.ldx_imm(0)
    p.symbol("argv0_buf.top")
    p.lda_abs(XSTACK)
    p.sta_abx(ARGV0)
    p.beq("argv0_buf.end")
    p.inx()
    p.bne("argv0_buf.top")
    p.symbol("argv0_buf.end")
    p.stx_abs(ARGN)
    p.call(OP_DROP_XSTACK)
    p.rts()

    def pusher(name, buf, count):
        p.symbol(name)
        p.ldx_abs(count)
        p.symbol(name + ".top")
        p.cpx_imm(0)
        with p.branch("bne"):
            p.rts()
        p.dex()
        p.lda_abx(buf)
        p.sta_abs(XSTACK)
        p.jmp_abs(name + ".top")

    # push_pfx goes on top of a string already pushed, so it pushes no
    # terminator.
    pusher("push_pfx", PFX, PFXN)
    pusher("push_cwd.chars", CWD, CWDN)
    pusher("push_argv0.chars", ARGV0, ARGN)

    for name in ("push_cwd", "push_argv0"):
        p.symbol(name)
        p.push(0)
        p.jmp_abs(name + ".chars")

    # A failed call returns -1, and the errno it leaves is ERRA or ERRB.
    p.symbol("errno_is")
    p.cpx_imm(0xFF)
    p.bne("errno_is.no")
    p.lda_abs(API_ERRNO + 1)
    p.bne("errno_is.no")
    p.lda_abs(API_ERRNO)
    p.cmp_abs(ERRA)
    p.beq("errno_is.yes")
    p.cmp_abs(ERRB)
    p.bne("errno_is.no")
    p.symbol("errno_is.yes")
    p.lda_imm(0)
    p.rts()
    p.symbol("errno_is.no")
    p.lda_imm(1)
    p.rts()

    # stat of a root returns one fixed entry: size 0, zero dates, a directory
    # named /. A machine without stat fails with the errno expected.
    p.symbol("stat_root")
    p.stx_abs(TMP)
    p.ora_abs(TMP)
    with p.branch("beq"):
        p.jmp_abs("errno_is")
    p.store(BAD, 0)
    p.ldx_imm(12)
    p.symbol("stat_root.zero")
    p.lda_abs(XSTACK)
    p.ora_abs(BAD)
    p.sta_abs(BAD)
    p.dex()
    p.bne("stat_root.zero")
    p.lda_abs(XSTACK)
    p.cmp_imm(0x10)
    with p.branch("beq"):
        p.store(BAD, 1)
    p.ldx_imm(13)
    p.symbol("stat_root.alt")
    p.lda_abs(XSTACK)
    p.dex()
    p.bne("stat_root.alt")
    p.lda_abs(XSTACK)
    p.cmp_imm(ord("/"))
    with p.branch("beq"):
        p.store(BAD, 1)
    p.lda_abs(XSTACK)
    with p.branch("beq"):
        p.store(BAD, 1)
    p.lda_abs(BAD)
    p.rts()

    # Reads GAP bytes, which must all be zero.
    p.symbol("rd_zero")
    p.store(BAD, 0)
    p.pushw(GAP)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)
    p.cmp_imm(GAP)
    with p.branch("beq"):
        p.store(BAD, 1)
    p.cpx_imm(0)
    with p.branch("beq"):
        p.store(BAD, 1)
    p.ldx_imm(GAP)
    p.symbol("rd_zero.top")
    p.lda_abs(XSTACK)
    with p.branch("beq"):
        p.store(BAD, 1)
    p.dex()
    p.bne("rd_zero.top")
    p.lda_abs(BAD)
    p.rts()

    p.symbol("main")

    text = p.say

    def open_it(name, flags):
        p.push_str(name)
        p.store(API_A, flags)
        p.call(OP_OPEN)
        p.sta_abs(FD)

    def record(platform=False):
        p.jsr_abs("record")
        checks[0] += 1
        if platform:
            platform_only.append(checks[0])

    def check(sub, platform=False):
        p.jsr_abs(sub)
        record(platform)

    def expect16(v):
        p.store(EXPL, v & 0xFF)
        p.store(EXPH, v >> 8)

    def rw0_at(addr):
        p.store(RW0_ADDR, addr & 0xFF)
        p.store(RW0_ADDR + 1, addr >> 8)

    def xram_io(op, addr, size):
        p.pushw(addr)
        p.pushw(size)
        p.lda_abs(FD)
        p.sta_abs(API_A)
        p.call(op)

    def seek_set(ofs):
        p.pushl(ofs)
        p.push(SEEK_SET)
        p.lda_abs(FD)
        p.sta_abs(API_A)
        p.call(OP_LSEEK)

    # Stored before the call, because the stores go through A.
    def expect_errno(a, b=None):
        p.store(ERRA, a)
        p.store(ERRB, a if b is None else b)

    for a, v in ((PASSN, 0), (FAILN, 0), (TIDX, 1), (VAL, 13), (BAD, 0)):
        p.store(a, v)
    p.store(API_A, ERRNO_OPT_LLVM)
    p.call(OP_ERRNO_OPT)

    text("FS ")

    open_it(NAME, O_RDONLY)
    p.lda_abs(FD)
    check("is_ff")

    open_it(NAME, O_WRONLY | O_CREAT | O_TRUNC)
    p.lda_abs(FD)
    check("not_ff")

    p.store(EXPL, 0)
    p.store(EXPH, 0)
    p.jsr_abs("seek_end")
    check("eq16")

    p.store(VAL, 13)
    check("wr_chunk")

    p.jsr_abs("do_close")
    check("not_ff")

    open_it(NAME, O_RDONLY)
    p.lda_abs(FD)
    check("not_ff")

    p.store(EXPL, CHUNK)
    p.store(EXPH, 0)
    p.jsr_abs("seek_end")
    check("eq16")

    for a, v in ((VAL, 13),):
        p.store(a, v)
    p.push(0)
    p.push(0)
    p.push(0)
    p.push(0)
    p.push(SEEK_SET)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_LSEEK)
    check("rd_chunk")

    p.store(EXPL, 0)
    p.store(EXPH, 0)
    p.push(CHUNK >> 8)
    p.push(CHUNK & 0xFF)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)
    check("eq16")

    p.jsr_abs("do_close")

    open_it(NAME, O_WRONLY | O_APPEND)
    p.lda_abs(FD)
    check("not_ff")

    p.store(VAL, (13 + 7 * CHUNK + 37) & 0xFF)
    check("wr_chunk")
    p.jsr_abs("do_close")

    open_it(NAME, O_RDONLY)
    p.store(EXPL, (2 * CHUNK) & 0xFF)
    p.store(EXPH, (2 * CHUNK) >> 8)
    p.jsr_abs("seek_end")
    check("eq16")

    p.store(VAL, 13)
    p.push(0)
    p.push(0)
    p.push(0)
    p.push(0)
    p.push(SEEK_SET)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_LSEEK)
    check("rd_chunk")

    check("rd_chunk")
    p.jsr_abs("do_close")

    open_it(NAME, O_WRONLY | O_TRUNC)
    p.store(EXPL, 0)
    p.store(EXPH, 0)
    p.jsr_abs("seek_end")
    check("eq16")
    p.jsr_abs("do_close")

    open_it(NAME, O_RDONLY)
    p.store(EXPL, 0)
    p.store(EXPH, 0)
    p.jsr_abs("seek_end")
    check("eq16")
    p.jsr_abs("do_close")

    open_it(NAME, O_WRONLY | O_TRUNC)
    p.store(VAL, 13)
    p.store(BAD, 0)
    p.store(CNT, CHUNKS)
    p.symbol("wr_all")
    p.jsr_abs("wr_chunk")
    p.cmp_imm(0x00)
    with p.branch("beq"):
        p.inc_abs(BAD)
    p.dec_abs(CNT)
    p.lda_abs(CNT)
    p.bne("wr_all")
    p.lda_abs(BAD)
    record()
    p.jsr_abs("do_close")

    open_it(NAME, O_RDONLY)
    p.store(EXPL, TOTAL & 0xFF)
    p.store(EXPH, TOTAL >> 8)
    p.jsr_abs("seek_end")
    check("eq16")

    p.store(VAL, 13)
    p.store(BAD, 0)
    p.store(CNT, CHUNKS)
    p.push(0)
    p.push(0)
    p.push(0)
    p.push(0)
    p.push(SEEK_SET)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_LSEEK)
    p.symbol("rd_all")
    p.jsr_abs("rd_chunk")
    p.cmp_imm(0x00)
    with p.branch("beq"):
        p.inc_abs(BAD)
    p.dec_abs(CNT)
    p.lda_abs(CNT)
    p.bne("rd_all")
    p.lda_abs(BAD)
    record()
    p.jsr_abs("do_close")

    open_it(NAME, O_RDONLY)
    p.store(EXPL, CHUNK)
    p.store(EXPH, 0)
    p.push(0)
    p.push(0)
    p.push(CHUNK >> 8)
    p.push(CHUNK & 0xFF)
    p.push(SEEK_SET)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_LSEEK)
    check("eq16")

    p.store(VAL, (13 + 7 * CHUNK + 37) & 0xFF)
    check("rd_chunk")
    p.jsr_abs("do_close")

    open_it(NAME, O_WRONLY | O_CREAT | O_EXCL)
    p.lda_abs(FD)
    check("is_ff")

    open_it(NAME2, O_WRONLY | O_CREAT | O_EXCL)
    p.lda_abs(FD)
    check("not_ff")
    p.jsr_abs("do_close")

    p.store(BAD, 0)
    for i in range(8):
        open_it(f"SAVE:s{i}.dat", O_WRONLY | O_CREAT)
        p.lda_abs(FD)
        p.sta_abs(FDS + i)
        p.cmp_imm(0xFF)
        with p.branch("bne"):
            p.inc_abs(BAD)
    p.lda_abs(BAD)
    record()

    open_it("SAVE:s8.dat", O_WRONLY | O_CREAT)
    p.lda_abs(FD)
    check("is_ff", platform=True)

    p.store(BAD, 0)
    for i in range(8):
        p.lda_abs(FDS + i)
        p.sta_abs(API_A)
        p.call(OP_CLOSE)
        p.cmp_imm(0xFF)
        with p.branch("bne"):
            p.inc_abs(BAD)
    p.lda_abs(BAD)
    record()

    open_it("CON:", O_RDONLY)
    p.lda_abs(FD)
    check("not_ff")
    p.jsr_abs("do_close")

    open_it("TTY:", O_RDONLY)
    p.lda_abs(FD)
    check("not_ff")
    p.jsr_abs("do_close")

    open_it("", O_RDONLY)
    p.lda_abs(FD)
    check("is_ff")

    p.store(API_A, 0x0F)
    p.call(OP_CLOSE)
    check("is_ff")

    p.push(0)
    p.push(0x10)
    p.store(API_A, 0x0F)
    p.call(OP_READ_XSTACK)
    check("is_ff")

    open_it("nope.dat", O_WRONLY | O_TRUNC)
    p.lda_abs(FD)
    check("is_ff")

    open_it(NAME, O_RDONLY)
    p.push(0x5A)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_WRITE_XSTACK)
    check("is_ff")

    p.store(EXPL, CHUNK)
    p.store(EXPH, 0)
    p.store(VAL, 13)
    p.jsr_abs("rd_chunk")
    for _ in range(4):
        p.push(0)
    p.push(SEEK_CUR)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_LSEEK)
    check("eq16")
    p.jsr_abs("do_close")

    open_it("probe.dat", O_RDONLY)
    p.lda_abs(FD)
    check("is_ff")
    open_it("probe.dat", O_RDONLY)
    p.lda_abs(FD)
    check("is_ff")

    p.jsr_abs("cwd_buf")
    check("cwd_has_drive")

    open_it("SAVE:pfx.dat", O_WRONLY | O_CREAT | O_TRUNC)
    p.lda_abs(FD)
    check("not_ff")
    p.jsr_abs("do_close")
    p.push_str("/pfx.dat")
    p.jsr_abs("push_pfx")
    p.store(API_A, O_RDONLY)
    p.call(OP_OPEN)
    p.sta_abs(FD)
    p.lda_abs(FD)
    check("is_ff")
    p.push_str("/Saves/rp6502/common/pfx.dat")
    p.jsr_abs("push_pfx")
    p.store(API_A, O_RDONLY)
    p.call(OP_OPEN)
    p.sta_abs(FD)
    p.lda_abs(FD)
    check("not_ff", platform=True)
    p.jsr_abs("do_close")
    open_it("msc1:pfx.dat", O_RDONLY)
    p.lda_abs(FD)
    check("is_ff")

    # getcwd returns the length of the path plus one.
    expect16(len(POCKET_CWD) + 1)
    p.lda_abs(CWDL)
    p.ldx_abs(CWDH)
    check("eq16", platform=True)
    p.store(TMP, 0)
    for i, ch in enumerate(POCKET_CWD + "\0"):
        p.lda_abs(CWD + i)
        p.cmp_imm(ord(ch))
        with p.branch("beq"):
            p.inc_abs(TMP)
    p.lda_abs(TMP)
    check("is_zero", platform=True)

    p.push_str("/Saves/rp6502/common")
    p.call(OP_CHDIR)
    check("is_ff")

    p.push_str("")
    p.jsr_abs("push_pfx")
    p.call(OP_CHDRIVE)
    check("not_ff")

    # OP_TIME_GET pushes a 64-bit count of seconds, which is popped low
    # byte first. A nonzero byte 3 puts the time after mid-1970, long after
    # the Pocket's fallback of 43200 seconds for an unset clock, and bytes
    # 4 to 7 stay zero until 2106.
    p.call(OP_TIME_GET)
    p.store(TMP, 0)
    for _ in range(3):
        p.lda_abs(XSTACK)
    p.lda_abs(XSTACK)
    with p.branch("bne"):
        p.inc_abs(TMP)
    for _ in range(4):
        p.lda_abs(XSTACK)
        with p.branch("beq"):
            p.inc_abs(TMP)
    p.lda_abs(TMP)
    check("is_zero")

    def tm_sum(op):
        for _ in range(8):
            p.push(0)
        p.call(op)
        p.lda_imm(0)
        p.sta_abs(TMP)
        for _ in range(18):
            p.lda_abs(XSTACK)
            p.clc()
            p.adc_abs(TMP)
            p.sta_abs(TMP)

    tm_sum(OP_GMTIME)
    p.lda_abs(TMP)
    p.sta_abs(TMP2)
    tm_sum(OP_LOCALTIME)
    p.lda_abs(TMP)
    p.cmp_abs(TMP2)
    with p.branch("bne"):
        p.lda_imm(1)
        p.bra("tm_done")
    p.lda_imm(0)
    p.symbol("tm_done")
    check("is_zero")

    open_it(NAME2, O_RDONLY)
    p.call(OP_SYNCFS)
    check("not_ff")
    p.jsr_abs("do_close")

    p.store(VAL, 13)
    p.store(CNT, XCHUNKS)
    rw0_at(XWR)
    p.jsr_abs("xfill")
    open_it(NAME3, O_WRONLY | O_CREAT | O_TRUNC)
    expect16(XLEN)
    xram_io(OP_WRITE_XRAM, XWR, XLEN)
    check("eq16")
    p.jsr_abs("do_close")

    open_it(NAME3, O_RDONLY)
    expect16(XLEN)
    xram_io(OP_READ_XRAM, XRD, XLEN + CHUNK)
    check("eq16")

    p.store(VAL, 13)
    p.store(CNT, XCHUNKS)
    rw0_at(XRD)
    check("xcheck")

    expect16(0)
    xram_io(OP_READ_XRAM, XRD, CHUNK)
    check("eq16")
    p.jsr_abs("do_close")

    open_it("ROM:" + ASSET, O_RDONLY)
    p.lda_abs(FD)
    check("not_ff")

    expect16(ALEN)
    xram_io(OP_READ_XRAM, XROM, ALEN + CHUNK)
    check("eq16")

    p.store(VAL, 13)
    p.store(CNT, ACHUNKS)
    rw0_at(XROM)
    check("xcheck")

    expect16(0)
    xram_io(OP_READ_XRAM, XROM, CHUNK)
    check("eq16")
    p.jsr_abs("do_close")

    # SAVE: is one flat folder: one separator after the colon is dropped and
    # the device name may be in any case.
    for name in ("SAVE:/" + NAME[len("SAVE:"):],
                 "save:\\" + NAME[len("SAVE:"):]):
        open_it(name, O_RDONLY)
        p.lda_abs(FD)
        check("not_ff")
        p.jsr_abs("do_close")

    for name in REFUSED:
        expect_errno(EINVAL)
        open_it(name, O_WRONLY | O_CREAT)
        check("errno_is")

    open_it(LONGEST, O_WRONLY | O_CREAT | O_TRUNC)
    p.lda_abs(FD)
    check("not_ff")
    p.jsr_abs("do_close")

    # Only open accepts SAVE:. A machine without stat gives ENOSYS.
    expect_errno(ENODEV, ENOSYS)
    p.push_str(NAME)
    p.call(OP_STAT)
    check("errno_is")
    p.call(OP_DROP_XSTACK)

    expect_errno(EINVAL)
    open_it(NAME, O_CREAT)
    check("errno_is")

    for name in ("a/b:c", "a*b"):
        expect_errno(EINVAL)
        open_it(name, O_RDONLY)
        check("errno_is")

    expect_errno(EINVAL)
    p.push(0)
    p.ldx_imm(0)
    p.lda_imm(ord("x"))
    p.symbol("long_path")
    p.sta_abs(XSTACK)
    p.dex()
    p.bne("long_path")
    p.store(API_A, O_RDONLY)
    p.call(OP_OPEN)
    check("errno_is")

    p.jsr_abs("argv0_buf")
    p.jsr_abs("push_argv0")
    p.store(API_A, O_RDONLY)
    p.call(OP_OPEN)
    p.sta_abs(FD)
    check("not_ff")
    p.jsr_abs("do_close")

    # A seek past the end of a writable file fills the gap with zeros.
    open_it(NAME4, O_WRONLY | O_CREAT | O_TRUNC)
    expect16(GAP)
    seek_set(GAP)
    check("eq16")
    p.jsr_abs("do_close")
    open_it(NAME4, O_RDONLY)
    expect16(GAP)
    p.jsr_abs("seek_end")
    check("eq16")
    seek_set(0)
    check("rd_zero")
    p.jsr_abs("do_close")

    # A drive root has one fixed entry, and an empty path is refused.
    expect_errno(ENOSYS)
    p.push_str("/")
    p.jsr_abs("push_pfx")
    p.call(OP_STAT)
    check("stat_root")
    p.call(OP_DROP_XSTACK)

    expect_errno(EINVAL, ENOSYS)
    p.push_str("")
    p.call(OP_STAT)
    check("errno_is")
    p.call(OP_DROP_XSTACK)

    # A file open for writing can be opened again.
    open_it(NAME, O_WRONLY)
    p.lda_abs(FD)
    p.sta_abs(FD2)
    check("not_ff")
    open_it(NAME, O_RDONLY)
    p.lda_abs(FD)
    check("not_ff")
    p.jsr_abs("do_close")
    p.lda_abs(FD2)
    p.sta_abs(FD)
    p.jsr_abs("do_close")

    # An open of a directory gives EACCES.
    expect_errno(EACCES)
    p.jsr_abs("push_cwd")
    p.store(API_A, O_RDONLY)
    p.call(OP_OPEN)
    check("errno_is")

    # A rename to another drive gives ENODEV, or ENOSYS on a machine without
    # rename. RENAME takes the new name on top of the old one.
    expect_errno(ENODEV, ENOSYS)
    p.push_str(NAME[len("SAVE:"):])
    p.push_str("MSC9:" + NAME[len("SAVE:"):])
    p.call(OP_RENAME)
    check("errno_is")

    text("\r\nPASS ")
    p.lda_abs(PASSN)
    p.jsr_abs("puthex")
    text("/")
    p.lda_abs(TIDX)
    p.sec()
    p.sbc_imm(0x01)
    p.jsr_abs("puthex")
    text("\r\nBAD ")
    p.lda_abs(FAILN)
    with p.branch("beq"):
        p.ldx_imm(0)
        p.symbol("tally")
        p.lda_abx(FAILS)
        p.jsr_abs("puthex")  # puthex and putc leave X unchanged
        p.lda_imm(ord(" "))
        p.jsr_abs("putc")
        p.inx()
        p.cpx_abs(FAILN)
        p.bne("tally")
    text("\r\n")
    p.stp()
    CHECKS = checks[0]
    PLATFORM[:] = platform_only
    return p


def passed():
    build()
    return f"PASS {CHECKS:02X}/{CHECKS:02X}"


def drive(emu, rom, save_dir=None):
    """A host-backed drive fails the four platform checks, because it
    allows more than eight open files, its SAVE: folder is not
    /Saves/rp6502/common and its working directory is not the Pocket's. The
    run passes when exactly those four fail."""
    build()
    for name in CREATES:
        (pathlib.Path(save_dir or ".") / name).unlink(missing_ok=True)

    def body(e):
        e.cmd('wait "BAD "')
        e.cmd(f'peek ${FAILN:04X} ${len(PLATFORM):02X}')
        if PLATFORM:
            e.cmd(f'peek ${FAILS:04X} '
                  + " ".join(f"${i:02X}" for i in PLATFORM))
    return rp6502_script.drive(emu, rom, body, env={"TZ": TZ},
                               save_dir=save_dir)


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
        rom = image(build())
        rom.add_asset(ASSET, payload(ACHUNKS))
        n = rom.write(a.emit)
        print(f"fstest.rp6502 {n} bytes, {CHECKS} checks, {TOTAL} byte payload")
    if a.drive:
        return drive(a.emu, a.rom, a.save_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
