#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rp6502_script  # noqa: E402
from rp6502_asm import (API_A, API_ERRNO, OP_LSEEK, OP_OPEN, OP_READ_XSTACK,
                        O_RDONLY, XSTACK, Asm, putc, putnib, puthex)
from rp6502_rom import Rom

ASSET = "song"

MARK = 4
CHUNK = 64

# core/api/std.c maps cc65's whence values 2, 0 and 1 to SEEK_SET, SEEK_CUR
# and SEEK_END. Passing 0, which is SEEK_SET in the llvm-mos SDK, would seek
# from the current position, so the seek to offset 0 at the start of each pass
# would leave the file position where it was.
SEEK_SET_CC65 = 2

FD = 0x0200
SEEN = 0x0201
LAST = 0x0202
LO = 0x0203
HI = 0x0204


def payload(letter):
    return b"".join(
        f"{letter * 4} asset line {i:02d} {letter * 4}\r\n".encode()
        for i in range(24))


def prog(letter):
    """The messages are subroutines because say() emits five bytes per
    character, so a branch around an inline message longer than 25
    characters would exceed the 127-byte forward range of a 6502 branch."""
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

    p.cmp_imm(0xFF)
    p.bne("opened")
    p.jsr_abs("say_openfail")
    p.stp()

    p.symbol("opened")
    p.store(LAST, ord(letter))
    p.store(LO, 0)
    p.store(HI, 0)
    p.say("probe " + letter + " reading ROM:" + ASSET + "\r\n")

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

    # A read returns its count with the low byte in A and the high byte in X.
    # A count is at most 512, the size of the xstack, so X is $FF only when
    # the read fails and returns -1.
    p.cpx_imm(0xFF)
    p.bne("head_read")
    p.jsr_abs("say_readfail")
    p.jmp_abs("pass")

    # Every byte a read leaves on the xstack is popped, even when nothing
    # uses it, because the next lseek or read fails with EINVAL when its
    # arguments are pushed on top of bytes left there.
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

    p.symbol("head_full")
    p.lda_abs(XSTACK)
    p.sta_abs(SEEN)
    for _ in range(MARK - 1):
        p.lda_abs(XSTACK)

    p.lda_abs(SEEN)
    p.cmp_abs(LAST)
    p.beq("unchanged")
    p.sta_abs(LAST)
    p.jsr_abs("say_changed")

    p.symbol("unchanged")
    p.symbol("drain")
    p.push(0)
    p.push(CHUNK)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)
    p.cpx_imm(0xFF)
    p.beq("tick")
    p.tax()
    p.beq("tick")
    p.symbol("eat")
    p.lda_abs(XSTACK)
    p.dex()
    p.bne("eat")
    p.jmp_abs("drain")

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


def drive(emu, rom, save_dir=None):
    def body(e):
        e.cmd('wait "reading ROM:song"')
    return rp6502_script.drive(emu, rom, body, save_dir=save_dir)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    ap.add_argument("--variant", default="A", help="the letter its asset repeats")
    ap.add_argument("--drive", action="store_true",
                    help="run the ROM on the emulator and check what it says")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the .rp6502 --emit wrote")
    ap.add_argument("--save-dir", help="the folder behind SAVE:")
    a = ap.parse_args()
    letter = a.variant.upper()[:1]
    if a.emit:
        print(f"sleepasset-{letter.lower()}.rp6502 {image(letter).write(a.emit)} bytes")
    if a.drive:
        return drive(a.emu, a.rom, a.save_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
