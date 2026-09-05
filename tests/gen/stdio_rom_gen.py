#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The three standard streams, on a host that keeps them apart.
#
# Under --headless the emulator has no window: host stdin is the program's
# stdin, and what the program writes to fd 1 and fd 2 lands on host stdout
# and host stderr, apart. This program says one line on stderr, then reads
# its input a byte at a time and echoes it to stdout, prints eof when a read
# answers nothing, and exits with a code the shell can see -- so one run of
# it is a claim about all three streams and the exit status at once.

import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import (API_A, API_X, OP_READ_XSTACK, OP_WRITE_XSTACK, XSTACK,
                        Asm)  # noqa: E402
from rp6502_rom import image  # noqa: E402

OP_EXIT = 0xFF
EXIT_CODE = 3
ERR = "err\n"
EOF = "eof\n"
INPUT = "a\nbb\n"
OUTPUT = INPUT + EOF


def write_str(p, fd, s):
    """A string to a descriptor through the xstack, which grows down."""
    for c in reversed(s.encode("latin-1")):
        p.push(c)
    p.call_a(OP_WRITE_XSTACK, fd)


def prog():
    p = Asm()
    write_str(p, 2, ERR)

    # One byte per read: a cooked read hands out the line it holds a byte
    # at a time, and a byte popped off the xstack goes straight back on it
    # for the write. A read that answers nothing is the end of the input.
    p.symbol("next")
    p.push(0)
    p.push(1)
    p.call_a(OP_READ_XSTACK, 0)
    p.tax()
    with p.branch("beq"):
        p.lda_abs(XSTACK)
        p.sta_abs(XSTACK)
        p.call_a(OP_WRITE_XSTACK, 1)
        p.jmp_abs("next")
    write_str(p, 1, EOF)
    p.store(API_A, EXIT_CODE)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()
    return p


def drive(emu, rom):
    """Run it headless with the input on a pipe and read all three back.

    EMU_ECHO is the suite's terminal mirror onto stderr, which here would
    land on top of the stream under test."""
    env = {k: v for k, v in os.environ.items() if k != "EMU_ECHO"}
    r = subprocess.run(
        [str(emu), "--headless", "--phi2", "0", "--mute", "--seed", "1",
         "--fill", "0", str(rom)],
        input=INPUT, capture_output=True, text=True, env=env, timeout=60)
    ok = True
    for name, got, want in (("stdout", r.stdout, OUTPUT),
                            ("stderr", r.stderr, ERR),
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
                    help="run the ROM headless and read its streams back")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the .rp6502 --emit wrote")
    a = ap.parse_args()
    if a.emit:
        print(f"stdio.rp6502 {image(prog()).write(a.emit)} bytes")
    if a.drive:
        return drive(a.emu, a.rom)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
