#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The whole drive in one boot. Forty-eight checks the machine decides for
# itself, printed as one row of dots, one line naming whatever failed,
# and a count. Nothing here needs reading off against a table.
#
# It exists because the alternative was a probe per question and a
# photograph per probe. Every hard thing found in this platform so far
# had no loud failure — a create that answers with a descriptor and
# writes nothing, a resize that reports success and does not happen, a
# read that returns its own cap — so each case below compares against
# something the wrong answer could not produce.
#
# The payload is computed, never stored, and it must not repeat: reads
# land in a staging store nothing clears between transfers, so a read
# that quietly does nothing hands back the previous one's bytes. Seven
# per byte alone comes back around every 256, which would let a
# duplicated block pass; thirty-seven more per chunk stretches the
# period past anything this test writes. A file that returns at the
# right length full of the wrong bytes fails here.
#
# Leaves fs1.dat, fs2.dat, pfx.dat and s0..s7.dat in /Saves/rp6502/common/.

import argparse
import pathlib

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rp6502_script  # noqa: E402
from rp6502_asm import (API_A, OP_CHDIR, OP_CHDRIVE, OP_CLOSE, OP_GETCWD,
                        OP_GMTIME, OP_LOCALTIME, OP_LSEEK, OP_OPEN,
                        OP_READ_XSTACK, OP_SYNCFS, OP_TIME_GET,
                        OP_WRITE_XSTACK, O_APPEND, O_CREAT, O_EXCL, O_RDONLY,
                        O_TRUNC, O_WRONLY, SEEK_CUR, SEEK_END, SEEK_SET,
                        XSTACK, Asm, putc, puthex, putnib)
from rp6502_rom import image


FD = 0x0200
VAL = 0x0201    # the payload byte due at the current offset
TMP = 0x0202
PASSN = 0x0203
FAILN = 0x0204
TIDX = 0x0205
EXPL, EXPH = 0x0206, 0x0207
CNT = 0x0208
BAD = 0x0209
CNTL, CNTH = 0x020A, 0x020B
TMP2 = 0x020C
FAILS = 0x0210  # the indices that failed, printed at the end
FDS = 0x0230    # eight descriptors for the slot exhaustion check

CHUNK = 128
CHUNKS = 12     # 1536 bytes: three transfer windows and a bit
TOTAL = CHUNK * CHUNKS

NAME = "fs1.dat"
NAME2 = "fs2.dat"

# What a run leaves behind. The first checks are that a name is not there
# and that an exclusive create takes one, so a second run against the same
# directory answers differently unless these go first. The card the Pocket's
# bench runs against is fresh every time; a working directory is not.
CREATES = [NAME, NAME2, "pfx.dat"] + [f"s{i}.dat" for i in range(9)]

# One of the checks asks whether the timezone offset reaches the C library,
# which it can only answer where there is an offset: on a machine set to UTC
# the two clocks agree and the check reads that as the offset never arriving.
# The Pocket has one from its own menu, so the emulator is given one here
# rather than inheriting whatever the machine running the suite sits in --
# a CI runner sits in UTC. No DST rule, and three characters because a
# shorter zone name is what the library declines to parse.
TZ = "EST5"


# How many checks the program made, counted as it was written. The tally it
# prints is the same number, so the expectation a driver waits for is derived
# from the program rather than typed beside it.
CHECKS = 0

# The checks that ask about the platform rather than about the filesystem.
PLATFORM = []


def build():
    global CHECKS
    checks = [0]
    platform_only = []
    p = Asm()
    p.jmp_abs("main")

    # --- console ---
    p.use(putc, putnib, puthex)

    # --- record: A is zero for a pass ---
    p.symbol("record")
    p.cmp_imm(0x00)
    with p.branch("bne"):
        p.lda_imm(ord("."))
        p.jsr_abs("putc")
        p.inc_abs(PASSN)
        # bra. This was a bvc, on the reasoning that V is clear here — and V
        # is whatever the last BIT $FFE0 in putc left in it, which is the
        # console's RX-ready bit. Press a key while the test runs and every
        # later pass was also logged as a failure. The 65C02 has a real
        # unconditional branch; use it.
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

    # --- A must be $FF (the call had to fail) ---
    p.symbol("is_ff")
    p.cmp_imm(0xFF)
    with p.branch("beq"):
        p.lda_imm(1)
        p.rts()
    p.lda_imm(0)
    p.rts()

    # --- A must not be $FF ---
    p.symbol("not_ff")
    p.cmp_imm(0xFF)
    with p.branch("beq"):
        p.lda_imm(0)
        p.rts()
    p.lda_imm(1)
    p.rts()

    # --- A must be zero (a miss counter stayed empty) ---
    p.symbol("is_zero")
    p.cmp_imm(0x00)
    with p.branch("beq"):
        p.lda_imm(1)
        p.rts()
    p.lda_imm(0)
    p.rts()

    # --- X:A against EXPH:EXPL ---
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

    # --- close whatever FD holds ---
    p.symbol("do_close")
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_CLOSE)
    p.rts()

    # --- seek to the end; returns X:A and leaves them for eq16 ---
    p.symbol("seek_end")
    for _ in range(4):
        p.push(0)
    p.push(SEEK_END)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_LSEEK)
    p.rts()

    # --- one chunk out of the running payload value ---
    # Pushing reverses, so the last byte of the chunk goes first: that is
    # VAL + 7 * 127, and 889 is 121 with the carry thrown away.
    p.symbol("wr_chunk")
    # Before the call: the syscall answers in A and X, and any store on
    # the way to comparing them destroys the answer.
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
    # A is the count's low byte and X its high; anything but 128/0 is wrong.
    p.jsr_abs("eq16")
    p.pha()  # carry the verdict past the payload advance
    p.lda_abs(VAL)
    p.clc()
    p.adc_imm((7 * CHUNK + 37) & 0xFF)
    p.sta_abs(VAL)
    p.pla()
    p.rts()

    # --- read one chunk back and check every byte of it ---
    p.symbol("rd_chunk")
    p.store(BAD, 0)
    p.push(CHUNK >> 8)
    p.push(CHUNK & 0xFF)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)
    p.sta_abs(CNTL)
    p.stx_abs(CNTH)
    p.cpx_imm(0xFF)  # the call itself failed, stack is clean
    with p.branch("bne"):
        p.lda_imm(1)
        p.rts()
    # Whatever arrived has to come off the stack whether it matched or
    # not: xstack is the 6502's own pointer and the next syscall pops its
    # arguments from wherever this leaves it.
    p.ldx_abs(CNTL)
    with p.branch("beq"):
        p.symbol("rd_chunk.top")
        p.lda_abs(XSTACK)  # pops
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
    # A short read is its own failure, counted after the stack is clean.
    p.lda_abs(CNTL)
    p.cmp_imm(CHUNK)
    with p.branch("beq"):
        p.inc_abs(BAD)
    p.lda_abs(CNTH)
    with p.branch("beq"):
        p.inc_abs(BAD)
    p.lda_abs(BAD)
    p.rts()

    # --- main ---
    p.symbol("main")

    text = p.say

    def open_it(name, flags):
        p.push_str(name)
        p.store(API_A, flags)
        p.call(OP_OPEN)
        p.sta_abs(FD)

    def record(platform=False):
        """One verdict, counted here so the tally the program prints and the
        line a driver waits for are the same number. `platform` marks a check
        that asks about the machine it is running on rather than about the
        filesystem: how many files it can hold open, and where its card puts
        this program's directory. Those differ by platform on purpose, so a
        driver is told which they are instead of being told a number."""
        p.jsr_abs("record")
        checks[0] += 1
        if platform:
            platform_only.append(checks[0])

    def check(sub, platform=False):
        """Run a verdict routine and record it."""
        p.jsr_abs(sub)
        record(platform)

    for a, v in ((PASSN, 0), (FAILN, 0), (TIDX, 1), (VAL, 13), (BAD, 0)):
        p.store(a, v)

    text("FS ")

    # 01 a name that is not there must not open
    open_it(NAME, O_RDONLY)
    p.lda_abs(FD)
    check("is_ff")

    # 02 create it
    open_it(NAME, O_WRONLY | O_CREAT | O_TRUNC)
    p.lda_abs(FD)
    check("not_ff")

    # 03 a new file is empty — or the host will not hold one at zero
    p.store(EXPL, 0)
    p.store(EXPH, 0)
    p.jsr_abs("seek_end")
    check("eq16")

    # 04 write one chunk
    p.store(VAL, 13)
    check("wr_chunk")

    # 05 close
    p.jsr_abs("do_close")
    check("not_ff")

    # 06 reopen for reading
    open_it(NAME, O_RDONLY)
    p.lda_abs(FD)
    check("not_ff")

    # 07 the host agrees on the length
    p.store(EXPL, CHUNK)
    p.store(EXPH, 0)
    p.jsr_abs("seek_end")
    check("eq16")

    # 08 and on every byte in it
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

    # 09 reading at the end returns nothing rather than something
    p.store(EXPL, 0)
    p.store(EXPH, 0)
    p.push(CHUNK >> 8)
    p.push(CHUNK & 0xFF)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)
    check("eq16")

    p.jsr_abs("do_close")

    # 10 append lands after what was there
    open_it(NAME, O_WRONLY | O_APPEND)
    p.lda_abs(FD)
    check("not_ff")

    # 11 a second chunk, continuing the pattern
    p.store(VAL, (13 + 7 * CHUNK + 37) & 0xFF)
    check("wr_chunk")
    p.jsr_abs("do_close")

    # 12 two chunks are there now
    open_it(NAME, O_RDONLY)
    p.store(EXPL, (2 * CHUNK) & 0xFF)
    p.store(EXPH, (2 * CHUNK) >> 8)
    p.jsr_abs("seek_end")
    check("eq16")

    # 13 and the append did not disturb the first chunk
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

    # 14 nor the second
    check("rd_chunk")
    p.jsr_abs("do_close")

    # 15 truncate takes it back to nothing
    open_it(NAME, O_WRONLY | O_TRUNC)
    p.store(EXPL, 0)
    p.store(EXPH, 0)
    p.jsr_abs("seek_end")
    check("eq16")
    p.jsr_abs("do_close")

    # 16 and the host agrees after a reopen, which a write cannot fake
    open_it(NAME, O_RDONLY)
    p.store(EXPL, 0)
    p.store(EXPH, 0)
    p.jsr_abs("seek_end")
    check("eq16")
    p.jsr_abs("do_close")

    # 17 a payload across several transfer windows
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

    # 18 all of it is there
    open_it(NAME, O_RDONLY)
    p.store(EXPL, TOTAL & 0xFF)
    p.store(EXPH, TOTAL >> 8)
    p.jsr_abs("seek_end")
    check("eq16")

    # 19 every byte of it, which is what a silent resize would break
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

    # 20 seek to the middle and read from there
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

    # 21 and the bytes there are the ones that belong there
    p.store(VAL, (13 + 7 * CHUNK + 37) & 0xFF)
    check("rd_chunk")
    p.jsr_abs("do_close")

    # 22 exclusive creation refuses a name already taken
    open_it(NAME, O_WRONLY | O_CREAT | O_EXCL)
    p.lda_abs(FD)
    check("is_ff")

    # 23 and takes one that is not
    open_it(NAME2, O_WRONLY | O_CREAT | O_EXCL)
    p.lda_abs(FD)
    check("not_ff")
    p.jsr_abs("do_close")

    # 24 eight files open at once, one per data slot
    p.store(BAD, 0)
    for i in range(8):
        open_it(f"s{i}.dat", O_WRONLY | O_CREAT)
        p.lda_abs(FD)
        p.sta_abs(FDS + i)
        p.cmp_imm(0xFF)
        with p.branch("bne"):
            p.inc_abs(BAD)
    p.lda_abs(BAD)
    record()

    # 25 the ninth has nowhere to go — on a machine whose files are its data
    # slots. A host-backed drive has no such ceiling and opens it.
    open_it("s8.dat", O_WRONLY | O_CREAT)
    p.lda_abs(FD)
    check("is_ff", platform=True)

    # 26 and closing them gives the slots back
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

    # 27 the console devices are answered above the drive
    open_it("CON:", O_RDONLY)
    p.lda_abs(FD)
    check("not_ff")
    p.jsr_abs("do_close")

    # 28 and so is the terminal
    open_it("TTY:", O_RDONLY)
    p.lda_abs(FD)
    check("not_ff")
    p.jsr_abs("do_close")

    # 29 a name of nothing is not a name
    open_it("", O_RDONLY)
    p.lda_abs(FD)
    check("is_ff")

    # 30 a descriptor never handed out cannot be closed
    p.store(API_A, 0x0F)
    p.call(OP_CLOSE)
    check("is_ff")

    # 31 nor read from
    p.push(0)
    p.push(0x10)
    p.store(API_A, 0x0F)
    p.call(OP_READ_XSTACK)
    check("is_ff")

    # 32 truncate is not create: a missing name stays missing
    open_it("nope.dat", O_WRONLY | O_TRUNC)
    p.lda_abs(FD)
    check("is_ff")

    # 33 a read-only descriptor refuses to be written
    open_it(NAME, O_RDONLY)
    p.push(0x5A)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_WRITE_XSTACK)
    check("is_ff")

    # 34 and the position follows a read, which is the cursor working
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

    # 35 the presence probe: a plain open of a missing name fails...
    open_it("probe.dat", O_RDONLY)
    p.lda_abs(FD)
    check("is_ff")
    # 36 ...and fails again — the probe itself must leave nothing
    # behind, or trying save00..saveNN as a directory listing would
    # manufacture the files it was looking for
    open_it("probe.dat", O_RDONLY)
    p.lda_abs(FD)
    check("is_ff")

    # 37 the relative spellings are one file: create under the drive
    # prefix
    open_it("MSC0:pfx.dat", O_WRONLY | O_CREAT | O_TRUNC)
    p.lda_abs(FD)
    check("not_ff")
    p.jsr_abs("do_close")
    # 38 the slash names the card's root, not the working directory,
    # so the rooted spelling must miss
    open_it("msc0:/pfx.dat", O_RDONLY)
    p.lda_abs(FD)
    check("is_ff")
    # 39 the absolute spelling of the working directory finds it
    open_it("MSC0:/Saves/rp6502/common/pfx.dat", O_RDONLY)
    p.lda_abs(FD)
    check("not_ff", platform=True)
    p.jsr_abs("do_close")
    # 40 and bare
    open_it("pfx.dat", O_RDONLY)
    p.lda_abs(FD)
    check("not_ff")
    p.jsr_abs("do_close")
    # 41 a drive that is not 0 is not this drive
    open_it("msc1:pfx.dat", O_RDONLY)
    p.lda_abs(FD)
    check("is_ff")

    # 42 the working directory is pinned and synthetic: getcwd answers
    # len+1 — the expectation stored first, because storing it uses A
    p.store(EXPL, 27)
    p.store(EXPH, 0)
    p.call(OP_GETCWD)
    check("eq16", platform=True)
    # 43 ...and spells it exactly, popped in order, so appending a name
    # to it opens the same file the bare name does
    p.store(TMP, 0)
    for ch in "MSC0:/Saves/rp6502/common/":
        p.lda_abs(XSTACK)
        p.cmp_imm(ord(ch))
        with p.branch("beq"):
            p.inc_abs(TMP)
    p.lda_abs(TMP)
    check("is_zero", platform=True)

    # 44 chdir errors whatever it names, even the directory getcwd
    # just answered
    p.push_str("MSC0:/Saves/rp6502/common/")
    p.call(OP_CHDIR)
    check("is_ff")

    # 45 chdrive accepts the one drive there is
    p.push_str("MSC0:")
    p.call(OP_CHDRIVE)
    check("not_ff")

    # 46 the clock arrived and is sane. Not a particular instant: this
    # ROM has to give the same verdict on the bench, where the host
    # latches a billion seconds, and on a Pocket reading its own wall
    # clock. What both have and a machine whose RTC never came through
    # does not is a real 32-bit epoch — byte 3 set puts it past
    # mid-1970, which the 43200-second fallback cannot reach, and the
    # top four bytes stay clear until 2106. Popped little end first.
    p.call(OP_TIME_GET)
    p.store(TMP, 0)
    for _ in range(3):
        p.lda_abs(XSTACK)   # the low three bytes are whatever time it is
    p.lda_abs(XSTACK)
    with p.branch("bne"):   # byte 3 set, so the clock is really set
        p.inc_abs(TMP)
    for _ in range(4):
        p.lda_abs(XSTACK)
        with p.branch("beq"):  # and nothing above it
            p.inc_abs(TMP)
    p.lda_abs(TMP)
    check("is_zero")

    # 47 the offset actually reaches the C library. It can arrive
    # perfectly and localtime still read UTC: a zone name of under
    # three characters makes the library decline to parse it and say
    # nothing, which is what shipped once. Nothing here knows the
    # offset — only that the two clocks must not agree, which they
    # cannot unless the zone took. Summed over the whole wire struct
    # so no field order is assumed.
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
    with p.branch("bne"):                       # they differ
        p.lda_imm(1)
        p.bra("tm_done")
    p.lda_imm(0)
    p.symbol("tm_done")
    check("is_zero")

    # 48 syncfs drives the flush the bridge can now survive
    open_it(NAME2, O_RDONLY)
    p.call(OP_SYNCFS)
    check("not_ff")
    p.jsr_abs("do_close")

    # --- the tally ---
    text("\r\nPASS ")
    p.lda_abs(PASSN)
    p.jsr_abs("puthex")
    text("/")
    p.lda_abs(TIDX)
    p.sec()
    p.sbc_imm(0x01)  # TIDX counted one past the last
    p.jsr_abs("puthex")
    text("\r\nBAD ")
    p.lda_abs(FAILN)
    with p.branch("beq"):
        p.ldx_imm(0)
        p.symbol("tally")
        p.lda_abx(FAILS)
        p.jsr_abs("puthex")  # leaves X alone, and so does putc
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
    """The line the program prints when every check passed, in the hex
    puthex writes it as. Counted off the program, so a check added below
    moves what a driver waits for without anyone remembering to."""
    build()
    return f"PASS {CHECKS:02X}/{CHECKS:02X}"


def drive(emu, rom):
    """The other half of this file: the program above, watched.

    The Pocket's bench holds it to every check because the Pocket is what
    the platform ones describe. A host-backed drive answers four of them
    differently and correctly -- it has no data slots to run out of and its
    working directory is wherever the test stands -- so what is asserted
    here is that those four are the only ones that differ. The program
    leaves the indices it failed in memory, so the claim is exact rather
    than a count."""
    build()  # what it counted is what is asserted below
    for name in CREATES:
        pathlib.Path(name).unlink(missing_ok=True)

    def body(e):
        e.cmd('wait "BAD "')  # the tally is printed whether or not any failed
        e.cmd(f'peek ${FAILN:04X} ${len(PLATFORM):02X}')
        if PLATFORM:
            e.cmd(f'peek ${FAILS:04X} '
                  + " ".join(f"${i:02X}" for i in PLATFORM))
    return rp6502_script.drive(emu, rom, body, env={"TZ": TZ})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    ap.add_argument("--drive", action="store_true",
                    help="run the ROM on the emulator and check what it says")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the .rp6502 --emit wrote")
    a = ap.parse_args()
    if a.emit:
        n = image(build()).write(a.emit)
        print(f"fstest.rp6502 {n} bytes, {CHECKS} checks, {TOTAL} byte payload")
    if a.drive:
        return drive(a.emu, a.rom)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
