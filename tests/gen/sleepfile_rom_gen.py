#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Two things a sleep used to take, asked for by one program.
#
# The first is issue 183: a file open when the machine sleeps, read
# again when it wakes. The drive's slot belongs to the host and the host
# is gone for the duration, so a program finds out what became of its
# descriptor by being told its read failed.
#
# The second is issue 185: sixteen console bytes written and not yet
# read. The RIA's TX queue is sixteen deep and the blob had a hole where
# it should have been, so whatever stood in it when the machine stopped
# went with the session.
#
# So this reads the file forever and counts out loud while it does. The
# count is the console test: the numbers run without a gap, and a gap
# after a wake is exactly what went missing, in units of the five bytes
# each number costs. The file is the drive test: every chunk has to
# begin where a chunk begins, and a read that fails or lands crooked
# says so in a line of its own rather than in silence.
#
# The file it reads is --data's, which the card package ships as
# probe.dat in Saves. tests/host/pocket builds the same pattern in the
# bench, where the collision this catches actually reproduces.

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import (API_A, API_ERRNO, OP_LSEEK, OP_OPEN, OP_READ_XSTACK,
                        O_RDONLY, XSTACK, Asm, putc, putnib, puthex)
from rp6502_rom import image

NAME = "probe.dat"
CHUNK = 16

# The file is this, over and over, so a chunk that does not begin with
# its first byte is a read that landed crooked.
UNIT = b"0123456789ABCDEF"
UNITS = 64

# cc65's whence is its own: core/api/std.c translates 2=SET, 0=CUR, 1=END.
SEEK_SET_CC65 = 2

FD = 0x0200
LO = 0x0201   # the count, low byte
HI = 0x0202   # the count, high byte
HEAD = 0x0203  # the first byte of the chunk just read


def payload():
    return UNIT * UNITS


def prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex)

    def message(name, body):
        p.symbol(name)
        body()
        p.rts()

    message("say_openfail", lambda: (
        p.say("\r\nFILE OPEN FAILED e="),
        p.lda_abs(API_ERRNO), p.jsr_abs("puthex"), p.say("\r\n")))
    message("say_readfail", lambda: (
        p.say("\r\nREAD FAILED e="),
        p.lda_abs(API_ERRNO), p.jsr_abs("puthex"), p.say("\r\n")))
    message("say_crooked", lambda: (
        p.say("\r\nCHUNK CROOKED head="),
        p.lda_abs(HEAD), p.jsr_abs("puthex"), p.say("\r\n")))

    p.symbol("main")
    p.push_str(NAME)
    p.store(API_A, O_RDONLY)
    p.call(OP_OPEN)
    p.sta_abs(FD)
    p.cmp_imm(0xFF)
    p.bne("opened")
    p.jsr_abs("say_openfail")
    p.stp()

    p.symbol("opened")
    p.store(LO, 0)
    p.store(HI, 0)
    p.say("counting, reading " + NAME + "\r\n")

    # ---- one chunk ----
    p.symbol("loop")
    p.push(0)
    p.push(CHUNK)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)

    # api_return_ax: the count in A, its high byte in X, so -1 is X of $FF.
    p.cpx_imm(0xFF)
    p.bne("read_ok")
    p.jsr_abs("say_readfail")
    p.jmp_abs("loop")

    p.symbol("read_ok")
    p.tax()
    p.bne("have_bytes")
    # Nothing, and no error: the end of the file. Round again.
    p.symbol("rewind")
    for _ in range(4):
        p.push(0)
    p.push(SEEK_SET_CC65)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_LSEEK)
    p.jmp_abs("loop")

    # The head is the byte worth checking; the rest come off the stack
    # because they were pushed, not because anyone looks at them.
    p.symbol("have_bytes")
    p.lda_abs(XSTACK)
    p.sta_abs(HEAD)
    p.dex()
    p.beq("drained")
    p.symbol("drain")
    p.lda_abs(XSTACK)
    p.dex()
    p.bne("drain")

    p.symbol("drained")
    p.lda_abs(HEAD)
    p.cmp_imm(UNIT[0])
    p.beq("aligned")
    p.jsr_abs("say_crooked")

    # ---- count out loud ----
    p.symbol("aligned")
    p.inc_abs(LO)
    p.bne("show")
    p.inc_abs(HI)
    p.symbol("show")
    p.lda_abs(HI)
    p.jsr_abs("puthex")
    p.lda_abs(LO)
    p.jsr_abs("puthex")
    p.lda_imm(ord(" "))
    p.jsr_abs("putc")
    p.jmp_abs("loop")
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    ap.add_argument("--data", help="write the data file this ROM reads")
    a = ap.parse_args()
    if a.emit:
        print(f"sleepfile.rp6502 {image(prog()).write(a.emit)} bytes")
    if a.data:
        Path(a.data).write_bytes(payload())
        print(f"{a.data} {len(payload())} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
