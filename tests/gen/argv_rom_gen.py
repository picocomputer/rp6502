#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# What did the machine say this program is called?
#
# argv[0] is not the program's own idea of anything: the loader puts it
# there, and on the Pocket it can only come from asking the host what its
# ROM slot is bound to. So printing it back is how a test sees whether
# that ask worked, from the one side that cannot lie about it.
#
# It exists because the ask did not work. On hardware the host answered
# with the right path every time and the firmware kept one answer in ten
# -- Get File's response landed in the window, and the flag the firmware
# trusted to say a response had landed stayed clear. A wake compares the
# name it is running against the name the host reports, so an answer
# thrown away there is a wake that restages a ROM it already holds.
#
# Empty brackets are the failure. A path between them is the fix.

import argparse

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rp6502_script  # noqa: E402
from rp6502_asm import XSTACK, Asm, putc, putnib, puthex
from rp6502_rom import image

# op 0x08: argv onto the xstack, its byte count back in AX.
OP_ARGV = 0x08

# argv's strings are NUL-separated in the buffer, and a separator has to
# be seen to be counted.
BAR = ord("|")


def prog():
    p = Asm()
    p.jmp_abs("main")
    p.use(putc, putnib, puthex)
    p.symbol("main")

    p.say("argv[")
    p.call(OP_ARGV)

    # The count comes back in AX. argv is smaller than a page here, so
    # the low byte is the whole of it; a high byte that is not zero would
    # mean something has gone wrong well before this ROM can say so.
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


def drive(emu, rom):
    """On a host that knows the path, the brackets are not empty."""
    def body(e):
        e.cmd('wait ".rp6502|"')
    return rp6502_script.drive(emu, rom, body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    ap.add_argument("--drive", action="store_true",
                    help="run the ROM on the emulator and check what it says")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the .rp6502 --emit wrote")
    a = ap.parse_args()
    if a.emit:
        print(f"argv.rp6502 {image(prog()).write(a.emit)} bytes")
    if a.drive:
        return drive(a.emu, a.rom)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
