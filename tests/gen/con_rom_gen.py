#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The raw console, fed from the host's stdin.
#
# tty_rom_gen.py asks whether a byte typed at the machine reaches a program
# that opened TTY:, and drives it with the script channel's `type`. This asks
# the other half of the same question: whether the host's own stdin reaches
# that program. It could not before -- the feed watched the line editor, and
# a raw reader never arms it -- so a program reading the console as a device
# saw nothing a pipe sent it.
#
# The bytes come back on fd 1 rather than the terminal, so the claim is one
# a shell can check. A 0x03 rides in the middle: on a pipe it is a byte like
# the rest, echoed back and latching nothing, so the program also asks the
# Ctrl-C latch on its way out and fails if anything was there.

import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import (API_A, API_X, OP_CLOSE, OP_OPEN, OP_READ_XSTACK,
                        OP_WRITE_XSTACK, O_RDONLY, XSTACK, Asm)  # noqa: E402
from rp6502_rom import image  # noqa: E402

OP_EXIT = 0xFF
OP_ATTR_GET = 0x0A
ATTR_SIGINT = 0x08
FD = 0x0200
INPUT = "raw \x03 bytes."
END = "."
EXIT_CODE = 7
EXIT_SIGINT = 9


def prog():
    p = Asm()
    p.push_str("TTY:")
    p.lda_imm(O_RDONLY)
    p.sta_abs(API_A)
    p.call(OP_OPEN)
    p.sta_abs(FD)

    # One byte a time, because a raw read answers with whatever is queued
    # now and mostly that is nothing: the program is faster than the wire.
    p.symbol("poll")
    p.push(0)
    p.push(1)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)
    p.tax()
    with p.branch("beq"):
        p.lda_abs(XSTACK)
        p.sta_abs(0x0201)
        p.sta_abs(XSTACK)
        p.call_a(OP_WRITE_XSTACK, 1)
        p.lda_abs(0x0201)
        p.cmp_imm(ord(END))
        with p.branch("bne"):
            p.lda_abs(FD)
            p.sta_abs(API_A)
            p.call(OP_CLOSE)
            # The trampoline leaves the attribute in A: a Ctrl-C the pipe
            # was never allowed to raise.
            p.call_a(OP_ATTR_GET, ATTR_SIGINT)
            p.tax()
            with p.branch("beq"):
                p.store(API_A, EXIT_SIGINT)
                p.store(API_X, 0)
                p.call(OP_EXIT)
                p.stp()
            p.store(API_A, EXIT_CODE)
            p.store(API_X, 0)
            p.call(OP_EXIT)
            p.stp()
    p.jmp_abs("poll")
    return p


def drive(emu, rom):
    """Pipe the input in and read it back off fd 1."""
    env = {k: v for k, v in os.environ.items() if k != "EMU_ECHO"}
    r = subprocess.run(
        [str(emu), "--headless", "--phi2", "0", "--mute", "--seed", "1",
         "--fill", "0", str(rom)],
        input=INPUT, capture_output=True, text=True, env=env, timeout=60)
    ok = True
    for name, got, want in (("stdout", r.stdout, INPUT),
                            ("exit code", r.returncode, EXIT_CODE)):
        if got != want:
            print(f"{sys.argv[0]}: {name} {got!r}, expected {want!r}",
                  file=sys.stderr)
            ok = False
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--emit")
    ap.add_argument("--drive", action="store_true",
                    help="run the ROM headless with the input on a pipe")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the .rp6502 --emit wrote")
    a = ap.parse_args()
    if a.emit:
        print(f"con.rp6502 {image(prog()).write(a.emit)} bytes")
    if a.drive:
        return drive(a.emu, a.rom)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
