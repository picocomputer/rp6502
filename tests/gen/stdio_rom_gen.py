#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import (API_A, API_X, OP_EXIT, OP_READ_XSTACK, OP_WRITE_XSTACK,
                        XSTACK, Asm)  # noqa: E402
from rp6502_rom import image  # noqa: E402

EXIT_CODE = 3
ERR = "err\n"
EOF = "eof\n"
INPUT = "a\rbb\r"
# core/api/std.c follows each line read from stdin with a newline, whether a
# carriage return or a line feed ended the line in the input.
OUTPUT = "a\nbb\n" + EOF


def write_str(p, fd, s):
    for c in reversed(s.encode("latin-1")):
        p.push(c)
    p.call_a(OP_WRITE_XSTACK, fd)


def prog():
    p = Asm()
    write_str(p, 2, ERR)

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


def drive(emu, rom, save_dir=None):
    """EMU_ECHO is removed because it mirrors the console onto stderr, which
    is one of the streams under test."""
    env = {k: v for k, v in os.environ.items() if k != "EMU_ECHO"}
    save = ["--save-dir", str(save_dir)] if save_dir else []
    r = subprocess.run(
        [str(emu), "--headless", "--phi2", "0", "--mute", "--seed", "1",
         "--fill", "0", *save, str(rom)],
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
    ap.add_argument("--save-dir", help="the folder behind SAVE:")
    a = ap.parse_args()
    if a.emit:
        print(f"stdio.rp6502 {image(prog()).write(a.emit)} bytes")
    if a.drive:
        return drive(a.emu, a.rom, a.save_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
