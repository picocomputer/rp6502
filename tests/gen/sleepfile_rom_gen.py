#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The program prints a count so that console output lost across a sleep shows
# as a gap in the count after a wake. Each number is five console bytes, so
# the sixteen bytes that the RIA's TX queue in core/ria/regs.sv holds span at
# most four numbers.

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import (API_A, API_ERRNO, OP_LSEEK, OP_OPEN, OP_READ_XSTACK,
                        O_RDONLY, XSTACK, Asm, putc, putnib, puthex)
from rp6502_rom import image

NAME = "probe.dat"
CHUNK = 16

# UNIT is CHUNK bytes long, so each correct read begins with UNIT's first
# byte.
UNIT = b"0123456789ABCDEF"
UNITS = 64

# core/api/std.c maps cc65's whence values 2, 0 and 1 to SEEK_SET, SEEK_CUR
# and SEEK_END.
SEEK_SET_CC65 = 2

FD = 0x0200
LO = 0x0201
HI = 0x0202
HEAD = 0x0203


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

    p.symbol("loop")
    p.push(0)
    p.push(CHUNK)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)

    # A read returns its count with the low byte in A and the high byte in X.
    # A count is at most 512, the size of the xstack, so X is $FF only when
    # the read fails and returns -1.
    p.cpx_imm(0xFF)
    p.bne("read_ok")
    p.jsr_abs("say_readfail")
    p.jmp_abs("loop")

    p.symbol("read_ok")
    p.tax()
    p.bne("have_bytes")
    p.symbol("rewind")
    for _ in range(4):
        p.push(0)
    p.push(SEEK_SET_CC65)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_LSEEK)
    p.jmp_abs("loop")

    # The rest of the chunk is popped, even though nothing uses it, because
    # the next read fails with EINVAL when its arguments are pushed on top of
    # bytes left on the xstack.
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
