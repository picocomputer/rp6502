#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The exit code is 2 because the machine sets code 1 itself when an exec fails
# to boot.

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rp6502_asm import (API_A, API_X, OP_EXIT, RW0_ADDR, RW0_DATA, RW0_STEP,
                        Asm)  # noqa: E402
from rp6502_rom import image  # noqa: E402

KEYBOARD = 0xFF10
EXIT_CODE = 2


def prog():
    p = Asm()
    p.xreg(0, 0, 0, KEYBOARD)
    p.store(RW0_STEP, 0)
    p.store(RW0_ADDR, KEYBOARD & 0xFF)
    p.store(RW0_ADDR + 1, KEYBOARD >> 8)

    # Bit 0 of the bitmap is set while no key is down.
    p.symbol("idle")
    p.lda_abs(RW0_DATA)
    p.lsr_a()
    p.bcs("idle")
    p.symbol("held")
    p.lda_abs(RW0_DATA)
    p.lsr_a()
    p.bcc("held")

    p.store(API_A, EXIT_CODE)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.stp()
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    a = ap.parse_args()
    if a.emit:
        print(f"keyboard.rp6502 {image(prog()).write(a.emit)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
