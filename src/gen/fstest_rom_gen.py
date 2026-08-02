#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The whole drive in one boot. Forty-seven checks the machine decides for
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
import zlib
from pathlib import Path

from bigfile_rom_gen import (ORG, XSTACK, API_A, API_OP, API_CALL, RIA_TX,
                             RIA_READY, OP_OPEN, OP_CLOSE, OP_READ_XSTACK,
                             OP_WRITE_XSTACK, O_RDONLY, O_WRONLY, O_CREAT,
                             O_TRUNC, Prog)

O_APPEND = 0x40
O_EXCL = 0x80
OP_LSEEK = 0x1A
OP_CHDIR = 0x29
OP_CHDRIVE = 0x2A
OP_GETCWD = 0x2B
OP_SYNCFS = 0x1E
OP_TIME_GET = 0x3F
SEEK_CUR, SEEK_END, SEEK_SET = 0, 1, 2  # cc65's spelling, not POSIX's

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
FAILS = 0x0210  # the indices that failed, printed at the end
FDS = 0x0230    # eight descriptors for the slot exhaustion check

CHUNK = 128
CHUNKS = 12     # 1536 bytes: three transfer windows and a bit
TOTAL = CHUNK * CHUNKS

NAME = "fs1.dat"
NAME2 = "fs2.dat"


def build():
    p = Prog()
    p.emit(0x4C, 0x00, 0x00)
    jmp_main = 1

    # --- console ---
    putc = p.here()
    p.emit(0x48)
    p.emit(0x2C, RIA_READY & 0xFF, RIA_READY >> 8)
    p.emit(0x10, 0xFB)
    p.emit(0x68)
    p.sta(RIA_TX)
    p.rts()

    putnib = p.here()
    p.emit(0xC9, 0x0A)
    p.emit(0xB0, 0x06)
    p.emit(0x18)
    p.emit(0x69, ord("0"))
    p.jmp(putc)
    p.emit(0x18)
    p.emit(0x69, ord("A") - 10)
    p.jmp(putc)

    puthex = p.here()
    p.emit(0x48)
    p.emit(0x4A, 0x4A, 0x4A, 0x4A)
    p.jsr(putnib)
    p.emit(0x68)
    p.emit(0x29, 0x0F)
    p.jmp(putnib)

    # --- record: A is zero for a pass ---
    record = p.here()
    p.emit(0xC9, 0x00)
    rfail = p.branch(0xD0)
    p.lda(ord("."))
    p.jsr(putc)
    p.inc_abs(PASSN)
    # bra. This was a bvc, on the reasoning that V is clear here — and V
    # is whatever the last BIT $FFE0 in putc left in it, which is the
    # console's RX-ready bit. Press a key while the test runs and every
    # later pass was also logged as a failure. The 65C02 has a real
    # unconditional branch; use it.
    rdone = p.branch(0x80)
    p.close(rfail)
    p.lda(ord("X"))
    p.jsr(putc)
    p.emit(0xAE, FAILN & 0xFF, FAILN >> 8)  # ldx FAILN
    p.lda_abs(TIDX)
    p.emit(0x9D, FAILS & 0xFF, FAILS >> 8)  # sta FAILS,x
    p.inc_abs(FAILN)
    p.close(rdone)
    p.inc_abs(TIDX)
    p.rts()

    # --- A must be $FF (the call had to fail) ---
    is_ff = p.here()
    p.emit(0xC9, 0xFF)
    h = p.branch(0xF0)
    p.lda(1)
    p.rts()
    p.close(h)
    p.lda(0)
    p.rts()

    # --- A must not be $FF ---
    not_ff = p.here()
    p.emit(0xC9, 0xFF)
    h = p.branch(0xF0)
    p.lda(0)
    p.rts()
    p.close(h)
    p.lda(1)
    p.rts()

    # --- A must be zero (a miss counter stayed empty) ---
    is_zero = p.here()
    p.emit(0xC9, 0x00)
    h = p.branch(0xF0)
    p.lda(1)
    p.rts()
    p.close(h)
    p.lda(0)
    p.rts()

    # --- X:A against EXPH:EXPL ---
    eq16 = p.here()
    p.cmp_abs(EXPL)
    h1 = p.branch(0xD0)
    p.emit(0x8A)  # txa
    p.cmp_abs(EXPH)
    h2 = p.branch(0xD0)
    p.lda(0)
    p.rts()
    p.close(h1)
    p.close(h2)
    p.lda(1)
    p.rts()

    # --- close whatever FD holds ---
    do_close = p.here()
    p.lda_abs(FD)
    p.sta(API_A)
    p.call(OP_CLOSE)
    p.rts()

    # --- seek to the end; returns X:A and leaves them for eq16 ---
    seek_end = p.here()
    for _ in range(4):
        p.push(0)
    p.push(SEEK_END)
    p.lda_abs(FD)
    p.sta(API_A)
    p.call(OP_LSEEK)
    p.rts()

    # --- one chunk out of the running payload value ---
    # Pushing reverses, so the last byte of the chunk goes first: that is
    # VAL + 7 * 127, and 889 is 121 with the carry thrown away.
    wr_chunk = p.here()
    # Before the call: the syscall answers in A and X, and any store on
    # the way to comparing them destroys the answer.
    p.store(EXPL, CHUNK)
    p.store(EXPH, 0)
    p.lda_abs(VAL)
    p.emit(0x18)
    p.emit(0x69, (7 * (CHUNK - 1)) & 0xFF)
    p.sta(TMP)
    p.ldx(CHUNK)
    top = p.here()
    p.lda_abs(TMP)
    p.sta(XSTACK)
    p.emit(0x38)
    p.emit(0xE9, 0x07)
    p.sta(TMP)
    p.emit(0xCA)
    p.emit(0xD0, (top - (p.here() + 2)) & 0xFF)
    p.lda_abs(FD)
    p.sta(API_A)
    p.call(OP_WRITE_XSTACK)
    # A is the count's low byte and X its high; anything but 128/0 is wrong.
    p.jsr(eq16)
    p.emit(0x48)  # pha — carry the verdict past the payload advance
    p.lda_abs(VAL)
    p.emit(0x18)
    p.emit(0x69, (7 * CHUNK + 37) & 0xFF)
    p.sta(VAL)
    p.emit(0x68)
    p.rts()

    # --- read one chunk back and check every byte of it ---
    rd_chunk = p.here()
    p.store(BAD, 0)
    p.push(CHUNK >> 8)
    p.push(CHUNK & 0xFF)
    p.lda_abs(FD)
    p.sta(API_A)
    p.call(OP_READ_XSTACK)
    p.sta(CNTL)
    p.emit(0x8E, CNTH & 0xFF, CNTH >> 8)  # stx CNTH
    p.emit(0xE0, 0xFF)  # cpx #$FF — the call itself failed, stack is clean
    live = p.branch(0xD0)
    p.lda(1)
    p.rts()
    p.close(live)
    # Whatever arrived has to come off the stack whether it matched or
    # not: xstack is the 6502's own pointer and the next syscall pops its
    # arguments from wherever this leaves it.
    p.emit(0xAE, CNTL & 0xFF, CNTL >> 8)  # ldx CNTL
    empty = p.branch(0xF0)
    top = p.here()
    p.lda_abs(XSTACK)  # pops
    p.cmp_abs(VAL)
    good = p.branch(0xF0)
    p.emit(0xEE, BAD & 0xFF, BAD >> 8)
    p.close(good)
    p.lda_abs(VAL)
    p.emit(0x18)
    p.emit(0x69, 0x07)
    p.sta(VAL)
    p.emit(0xCA)
    p.emit(0xD0, (top - (p.here() + 2)) & 0xFF)
    p.close(empty)
    p.lda_abs(VAL)
    p.emit(0x18)
    p.emit(0x69, 37)
    p.sta(VAL)
    # A short read is its own failure, counted after the stack is clean.
    p.lda_abs(CNTL)
    p.emit(0xC9, CHUNK)
    lenok = p.branch(0xF0)
    p.emit(0xEE, BAD & 0xFF, BAD >> 8)
    p.close(lenok)
    p.lda_abs(CNTH)
    p.emit(0xF0, 0x03)
    p.emit(0xEE, BAD & 0xFF, BAD >> 8)
    p.lda_abs(BAD)
    p.rts()

    # --- main ---
    main = p.here()
    p.b[jmp_main] = main & 0xFF
    p.b[jmp_main + 1] = main >> 8

    def text(s):
        for c in s.encode():
            p.lda(c)
            p.jsr(putc)

    def open_it(name, flags):
        p.push_str(name)
        p.store(API_A, flags)
        p.call(OP_OPEN)
        p.sta(FD)

    def check(sub):
        """Run a verdict routine and record it."""
        p.jsr(sub)
        p.jsr(record)

    for a, v in ((PASSN, 0), (FAILN, 0), (TIDX, 1), (VAL, 13), (BAD, 0)):
        p.store(a, v)

    text("FS ")

    # 01 a name that is not there must not open
    open_it(NAME, O_RDONLY)
    p.lda_abs(FD)
    check(is_ff)

    # 02 create it
    open_it(NAME, O_WRONLY | O_CREAT | O_TRUNC)
    p.lda_abs(FD)
    check(not_ff)

    # 03 a new file is empty — or the host will not hold one at zero
    p.store(EXPL, 0)
    p.store(EXPH, 0)
    p.jsr(seek_end)
    check(eq16)

    # 04 write one chunk
    p.store(VAL, 13)
    check(wr_chunk)

    # 05 close
    p.jsr(do_close)
    check(not_ff)

    # 06 reopen for reading
    open_it(NAME, O_RDONLY)
    p.lda_abs(FD)
    check(not_ff)

    # 07 the host agrees on the length
    p.store(EXPL, CHUNK)
    p.store(EXPH, 0)
    p.jsr(seek_end)
    check(eq16)

    # 08 and on every byte in it
    for a, v in ((VAL, 13),):
        p.store(a, v)
    p.push(0)
    p.push(0)
    p.push(0)
    p.push(0)
    p.push(SEEK_SET)
    p.lda_abs(FD)
    p.sta(API_A)
    p.call(OP_LSEEK)
    check(rd_chunk)

    # 09 reading at the end returns nothing rather than something
    p.store(EXPL, 0)
    p.store(EXPH, 0)
    p.push(CHUNK >> 8)
    p.push(CHUNK & 0xFF)
    p.lda_abs(FD)
    p.sta(API_A)
    p.call(OP_READ_XSTACK)
    check(eq16)

    p.jsr(do_close)

    # 10 append lands after what was there
    open_it(NAME, O_WRONLY | O_APPEND)
    p.lda_abs(FD)
    check(not_ff)

    # 11 a second chunk, continuing the pattern
    p.store(VAL, (13 + 7 * CHUNK + 37) & 0xFF)
    check(wr_chunk)
    p.jsr(do_close)

    # 12 two chunks are there now
    open_it(NAME, O_RDONLY)
    p.store(EXPL, (2 * CHUNK) & 0xFF)
    p.store(EXPH, (2 * CHUNK) >> 8)
    p.jsr(seek_end)
    check(eq16)

    # 13 and the append did not disturb the first chunk
    p.store(VAL, 13)
    p.push(0)
    p.push(0)
    p.push(0)
    p.push(0)
    p.push(SEEK_SET)
    p.lda_abs(FD)
    p.sta(API_A)
    p.call(OP_LSEEK)
    check(rd_chunk)

    # 14 nor the second
    check(rd_chunk)
    p.jsr(do_close)

    # 15 truncate takes it back to nothing
    open_it(NAME, O_WRONLY | O_TRUNC)
    p.store(EXPL, 0)
    p.store(EXPH, 0)
    p.jsr(seek_end)
    check(eq16)
    p.jsr(do_close)

    # 16 and the host agrees after a reopen, which a write cannot fake
    open_it(NAME, O_RDONLY)
    p.store(EXPL, 0)
    p.store(EXPH, 0)
    p.jsr(seek_end)
    check(eq16)
    p.jsr(do_close)

    # 17 a payload across several transfer windows
    open_it(NAME, O_WRONLY | O_TRUNC)
    p.store(VAL, 13)
    p.store(BAD, 0)
    p.store(CNT, CHUNKS)
    top = p.here()
    p.jsr(wr_chunk)
    p.emit(0xC9, 0x00)
    wok = p.branch(0xF0)
    p.emit(0xEE, BAD & 0xFF, BAD >> 8)
    p.close(wok)
    p.emit(0xCE, CNT & 0xFF, CNT >> 8)  # dec CNT
    p.lda_abs(CNT)
    p.emit(0xD0, (top - (p.here() + 2)) & 0xFF)
    p.lda_abs(BAD)
    p.jsr(record)
    p.jsr(do_close)

    # 18 all of it is there
    open_it(NAME, O_RDONLY)
    p.store(EXPL, TOTAL & 0xFF)
    p.store(EXPH, TOTAL >> 8)
    p.jsr(seek_end)
    check(eq16)

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
    p.sta(API_A)
    p.call(OP_LSEEK)
    top = p.here()
    p.jsr(rd_chunk)
    p.emit(0xC9, 0x00)
    rok = p.branch(0xF0)
    p.emit(0xEE, BAD & 0xFF, BAD >> 8)
    p.close(rok)
    p.emit(0xCE, CNT & 0xFF, CNT >> 8)
    p.lda_abs(CNT)
    p.emit(0xD0, (top - (p.here() + 2)) & 0xFF)
    p.lda_abs(BAD)
    p.jsr(record)
    p.jsr(do_close)

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
    p.sta(API_A)
    p.call(OP_LSEEK)
    check(eq16)

    # 21 and the bytes there are the ones that belong there
    p.store(VAL, (13 + 7 * CHUNK + 37) & 0xFF)
    check(rd_chunk)
    p.jsr(do_close)

    # 22 exclusive creation refuses a name already taken
    open_it(NAME, O_WRONLY | O_CREAT | O_EXCL)
    p.lda_abs(FD)
    check(is_ff)

    # 23 and takes one that is not
    open_it(NAME2, O_WRONLY | O_CREAT | O_EXCL)
    p.lda_abs(FD)
    check(not_ff)
    p.jsr(do_close)

    # 24 eight files open at once, one per data slot
    p.store(BAD, 0)
    for i in range(8):
        open_it(f"s{i}.dat", O_WRONLY | O_CREAT)
        p.lda_abs(FD)
        p.emit(0x8D, (FDS + i) & 0xFF, (FDS + i) >> 8)
        p.emit(0xC9, 0xFF)
        h = p.branch(0xD0)
        p.emit(0xEE, BAD & 0xFF, BAD >> 8)
        p.close(h)
    p.lda_abs(BAD)
    p.jsr(record)

    # 25 the ninth has nowhere to go
    open_it("s8.dat", O_WRONLY | O_CREAT)
    p.lda_abs(FD)
    check(is_ff)

    # 26 and closing them gives the slots back
    p.store(BAD, 0)
    for i in range(8):
        p.emit(0xAD, (FDS + i) & 0xFF, (FDS + i) >> 8)
        p.sta(API_A)
        p.call(OP_CLOSE)
        p.emit(0xC9, 0xFF)
        h = p.branch(0xD0)
        p.emit(0xEE, BAD & 0xFF, BAD >> 8)
        p.close(h)
    p.lda_abs(BAD)
    p.jsr(record)

    # 27 the console devices are answered above the drive
    open_it("CON:", O_RDONLY)
    p.lda_abs(FD)
    check(not_ff)
    p.jsr(do_close)

    # 28 and so is the terminal
    open_it("TTY:", O_RDONLY)
    p.lda_abs(FD)
    check(not_ff)
    p.jsr(do_close)

    # 29 a name of nothing is not a name
    open_it("", O_RDONLY)
    p.lda_abs(FD)
    check(is_ff)

    # 30 a descriptor never handed out cannot be closed
    p.store(API_A, 0x0F)
    p.call(OP_CLOSE)
    check(is_ff)

    # 31 nor read from
    p.push(0)
    p.push(0x10)
    p.store(API_A, 0x0F)
    p.call(OP_READ_XSTACK)
    check(is_ff)

    # 32 truncate is not create: a missing name stays missing
    open_it("nope.dat", O_WRONLY | O_TRUNC)
    p.lda_abs(FD)
    check(is_ff)

    # 33 a read-only descriptor refuses to be written
    open_it(NAME, O_RDONLY)
    p.push(0x5A)
    p.lda_abs(FD)
    p.sta(API_A)
    p.call(OP_WRITE_XSTACK)
    check(is_ff)

    # 34 and the position follows a read, which is the cursor working
    p.store(EXPL, CHUNK)
    p.store(EXPH, 0)
    p.store(VAL, 13)
    p.jsr(rd_chunk)
    for _ in range(4):
        p.push(0)
    p.push(SEEK_CUR)
    p.lda_abs(FD)
    p.sta(API_A)
    p.call(OP_LSEEK)
    check(eq16)
    p.jsr(do_close)

    # 35 the presence probe: a plain open of a missing name fails...
    open_it("probe.dat", O_RDONLY)
    p.lda_abs(FD)
    check(is_ff)
    # 36 ...and fails again — the probe itself must leave nothing
    # behind, or trying save00..saveNN as a directory listing would
    # manufacture the files it was looking for
    open_it("probe.dat", O_RDONLY)
    p.lda_abs(FD)
    check(is_ff)

    # 37 the relative spellings are one file: create under the drive
    # prefix
    open_it("MSC0:pfx.dat", O_WRONLY | O_CREAT | O_TRUNC)
    p.lda_abs(FD)
    check(not_ff)
    p.jsr(do_close)
    # 38 the slash names the card's root, not the working directory,
    # so the rooted spelling must miss
    open_it("msc0:/pfx.dat", O_RDONLY)
    p.lda_abs(FD)
    check(is_ff)
    # 39 the absolute spelling of the working directory finds it
    open_it("MSC0:/Saves/rp6502/common/pfx.dat", O_RDONLY)
    p.lda_abs(FD)
    check(not_ff)
    p.jsr(do_close)
    # 40 and bare
    open_it("pfx.dat", O_RDONLY)
    p.lda_abs(FD)
    check(not_ff)
    p.jsr(do_close)
    # 41 a drive that is not 0 is not this drive
    open_it("msc1:pfx.dat", O_RDONLY)
    p.lda_abs(FD)
    check(is_ff)

    # 42 the working directory is pinned and synthetic: getcwd answers
    # len+1 — the expectation stored first, because storing it uses A
    p.store(EXPL, 27)
    p.store(EXPH, 0)
    p.call(OP_GETCWD)
    check(eq16)
    # 43 ...and spells it exactly, popped in order, so appending a name
    # to it opens the same file the bare name does
    p.store(TMP, 0)
    for ch in "MSC0:/Saves/rp6502/common/":
        p.lda_abs(XSTACK)
        p.emit(0xC9, ord(ch))
        h = p.branch(0xF0)
        p.inc_abs(TMP)
        p.close(h)
    p.lda_abs(TMP)
    check(is_zero)

    # 44 chdir errors whatever it names, even the directory getcwd
    # just answered
    p.push_str("MSC0:/Saves/rp6502/common/")
    p.call(OP_CHDIR)
    check(is_ff)

    # 45 chdrive accepts the one drive there is
    p.push_str("MSC0:")
    p.call(OP_CHDRIVE)
    check(not_ff)

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
    h = p.branch(0xD0)      # bne: byte 3 set, so the clock is really set
    p.inc_abs(TMP)
    p.close(h)
    for _ in range(4):
        p.lda_abs(XSTACK)
        h = p.branch(0xF0)  # beq: and nothing above it
        p.inc_abs(TMP)
        p.close(h)
    p.lda_abs(TMP)
    check(is_zero)

    # 47 syncfs drives the flush the bridge can now survive
    open_it(NAME2, O_RDONLY)
    p.call(OP_SYNCFS)
    check(not_ff)
    p.jsr(do_close)

    # --- the tally ---
    text("\r\nPASS ")
    p.lda_abs(PASSN)
    p.jsr(puthex)
    text("/")
    p.lda_abs(TIDX)
    p.emit(0x38)
    p.emit(0xE9, 0x01)  # TIDX counted one past the last
    p.jsr(puthex)
    text("\r\nBAD ")
    p.lda_abs(FAILN)
    none = p.branch(0xF0)
    p.ldx(0)
    top = p.here()
    p.emit(0xBD, FAILS & 0xFF, FAILS >> 8)  # lda FAILS,x
    p.jsr(puthex)  # leaves X alone, and so does putc
    p.lda(ord(" "))
    p.jsr(putc)
    p.emit(0xE8)
    p.emit(0xEC, FAILN & 0xFF, FAILN >> 8)
    p.emit(0xD0, (top - (p.here() + 2)) & 0xFF)
    p.close(none)
    text("\r\n")
    p.emit(0xDB)
    return bytes(p.b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    a = ap.parse_args()
    if a.emit:
        body = build()
        rom = b"#!RP6502\n"
        for addr, data in ((ORG, body),
                           (0xFFFC, bytes((ORG & 0xFF, ORG >> 8)))):
            crc = zlib.crc32(data) & 0xFFFFFFFF
            rom += f"${addr:05X} ${len(data):X} ${crc:08X}\n".encode() + data
        Path(a.emit).write_bytes(rom)
        print(f"fstest.rp6502 {len(rom)} bytes, 47 checks, {TOTAL} byte payload")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
