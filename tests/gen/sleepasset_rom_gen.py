#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Does a ROM: asset still read what it read before the machine slept?
#
# A program's assets are not in the savestate. They are read on demand
# out of the staging store, which the blob does not carry and the host
# refills on its own schedule, so the descriptor a restored session holds
# points into whatever the wake put there. Streaming music is where this
# shows first, because it is the one thing that keeps reading all session.
#
# So this is music's stand-in, and it reads exactly like music does: the
# asset walked end to end, over and over, forever. What it does NOT do is
# print what it reads. The console is four bytes per host command, and a
# flooded one comes back interleaved with itself -- measured on hardware,
# where "streaming ROM:song" arrived as "stsreamingt ROM:sorng" -- so a
# program that prints every byte destroys the log it is being read from,
# including the firmware probe's own readings.
#
# It prints when something changes instead. Every pass checks the four
# bytes at the head of the asset against the letter this variant was
# built with, and says so only when that letter changes or a read fails.
# One line at the moment of the break is worth more than a screen of
# text nobody can trust.

import argparse

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rp6502_script  # noqa: E402
from rp6502_asm import (API_A, API_ERRNO, OP_LSEEK, OP_OPEN, OP_READ_XSTACK,
                        O_RDONLY, XSTACK, Asm, putc, putnib, puthex)
from rp6502_rom import Rom

ASSET = "song"

# The head of every line, and what a pass checks.
MARK = 4
# The drain's read size. Bigger than MARK only to keep the syscall count
# down; nothing looks at these bytes.
CHUNK = 64

# cc65's whence is its own: core/api/std.c:410 translates 2=SET, 0=CUR,
# 1=END. Spelling SET as the POSIX 0 asks for CUR, which rewinds nothing
# and leaves every pass after the first reading at the end of the file.
SEEK_SET_CC65 = 2

FD = 0x0200
SEEN = 0x0201  # the letter this pass read
LAST = 0x0202  # the letter the last reported pass read
LO = 0x0203    # pass counter, low
HI = 0x0204    # pass counter, high


def payload(letter):
    """The asset. Every line starts with the variant's letter four times,
    which is what a pass checks, and says it again in the middle so a
    human reading a screenshot can tell the two apart at a glance."""
    return b"".join(
        f"{letter * 4} asset line {i:02d} {letter * 4}\r\n".encode()
        for i in range(24))


def prog(letter):
    """The loop, plus its messages as subroutines.

    Every message is a jsr, because a 6502 branch reaches 127 bytes and
    p.say spends five per character -- one sentence inline and the branch
    around it no longer reaches."""
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex)

    def message(name, body):
        p.symbol(name)
        body()
        p.rts()

    message("say_openfail", lambda: (
        p.say("ASSET OPEN FAILED e="),
        p.lda_abs(API_ERRNO), p.jsr_abs("puthex"), p.say("\r\n")))
    message("say_readfail", lambda: (
        p.say("\r\nREAD FAILED e="),
        p.lda_abs(API_ERRNO), p.jsr_abs("puthex"), p.say("\r\n")))
    message("say_short", lambda: p.say("\r\nSHORT HEAD\r\n"))
    message("say_changed", lambda: (
        p.say("\r\n*** ASSET NOW READS "),
        p.lda_abs(SEEN), p.putc_a(),
        p.say(" (built " + letter + ") ***\r\n")))
    message("say_ok", lambda: (
        p.say("\r\n"), p.lda_abs(SEEN), p.putc_a(), p.say(" ok\r\n")))

    p.symbol("main")
    p.push_str("ROM:" + ASSET)
    p.store(API_A, O_RDONLY)
    p.call(OP_OPEN)
    p.sta_abs(FD)

    # An open that fails has nothing to watch, and saying so once beats a
    # screen of nothing.
    p.cmp_imm(0xFF)
    p.bne("opened")
    p.jsr_abs("say_openfail")
    p.stp()

    p.symbol("opened")
    p.store(LAST, ord(letter))
    p.store(LO, 0)
    p.store(HI, 0)
    p.say("probe " + letter + " reading ROM:" + ASSET + "\r\n")

    # ---- one pass: rewind, check the head, drain the rest ----
    p.symbol("pass")
    for _ in range(4):
        p.push(0)
    p.push(SEEK_SET_CC65)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_LSEEK)

    p.push(0)
    p.push(MARK)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)

    # api_return_ax puts the count in A with its high byte in X, so a
    # failure is X of $FF and not a very long read.
    p.cpx_imm(0xFF)
    p.bne("head_read")
    p.jsr_abs("say_readfail")
    # Not stopped: a failure that heals is worth seeing heal, and the
    # letter check is what says whether it did.
    p.jmp_abs("pass")

    # Short of the mark is its own answer, and popping what did arrive
    # keeps the stack straight for the next pass.
    p.symbol("head_read")
    p.cmp_imm(MARK)
    p.beq("head_full")
    p.tax()
    p.beq("head_none")
    p.symbol("drop")
    p.lda_abs(XSTACK)
    p.dex()
    p.bne("drop")
    p.symbol("head_none")
    p.jsr_abs("say_short")
    p.jmp_abs("pass")

    # The first byte is the letter; the rest come off the stack because
    # they were pushed, not because anyone looks at them.
    p.symbol("head_full")
    p.lda_abs(XSTACK)
    p.sta_abs(SEEN)
    for _ in range(MARK - 1):
        p.lda_abs(XSTACK)

    # Only a change is worth a line. This is the moment the bug happens,
    # and it prints once, where it happens.
    p.lda_abs(SEEN)
    p.cmp_abs(LAST)
    p.beq("unchanged")
    p.sta_abs(LAST)
    p.jsr_abs("say_changed")

    # ---- drain the rest of the asset, silently ----
    p.symbol("unchanged")
    p.symbol("drain")
    p.push(0)
    p.push(CHUNK)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)
    p.cpx_imm(0xFF)
    p.beq("tick")  # the head already reported; do not say it twice
    p.tax()
    p.beq("tick")  # no bytes: the end of the asset
    p.symbol("eat")
    p.lda_abs(XSTACK)
    p.dex()
    p.bne("eat")
    p.jmp_abs("drain")

    # ---- liveness, rationed ----
    # A dot every 256 passes and a line every sixteen dots, because the
    # console is a host command per four bytes and this runs forever.
    p.symbol("tick")
    p.inc_abs(LO)
    p.bne("next")
    p.lda_imm(ord("."))
    p.jsr_abs("putc")
    p.inc_abs(HI)
    p.lda_abs(HI)
    p.and_imm(0x0F)
    p.bne("next")
    p.jsr_abs("say_ok")
    p.symbol("next")
    p.jmp_abs("pass")
    return p


def image(letter):
    rom = Rom().program(prog(letter))
    rom.add_asset(ASSET, payload(letter))
    return rom


def drive(emu, rom):
    """The program above, watched. It says its letter once at the top and
    then goes quiet, so that line is the whole contract."""
    def body(e):
        e.cmd('wait "reading ROM:song"')
    return rp6502_script.drive(emu, rom, body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    ap.add_argument("--variant", default="A", help="the letter its asset repeats")
    ap.add_argument("--drive", action="store_true",
                    help="run the ROM on the emulator and check what it says")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the .rp6502 --emit wrote")
    a = ap.parse_args()
    letter = a.variant.upper()[:1]
    if a.emit:
        print(f"sleepasset-{letter.lower()}.rp6502 {image(letter).write(a.emit)} bytes")
    if a.drive:
        return drive(a.emu, a.rom)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
