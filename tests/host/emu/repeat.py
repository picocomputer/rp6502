#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""Runs with one seed answer cpu the same, though the program reads files as it
starts, because with no window a file transfer completes before the call
returns."""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "gen"))
import rp6502_script  # noqa: E402


def drive(emu, rom, save_dir=None):
    answers = []

    def body(e):
        e.cmd("run 20")
        answers.append(e.cmd("cpu"))
    # cpu ends with the seed, so the runs share one.
    for _ in range(3):
        code = rp6502_script.drive(emu, rom, body, args=("--seed", "1"),
                                   save_dir=save_dir)
        if code:
            return code
    if len(set(answers)) != 1:
        print(f"{sys.argv[0]}: the runs differ: {answers}", file=sys.stderr)
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--drive", action="store_true", help="run the ROM three times")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the ROM to run")
    ap.add_argument("--save-dir", help="the folder behind SAVE:")
    a = ap.parse_args()
    if a.drive:
        return drive(a.emu, a.rom, a.save_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
