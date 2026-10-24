#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""rp6502_rom writes the ROMs and runs the tests of a test file.

A test file names its ROMs in ROMS, a dict of ROM name to a function that
returns an Asm. It names its tests in TESTS, a dict of test name to a
function. A test fails when it raises. The file ends with main(globals()).

  python3 <file> --emit <folder>
    The file writes each ROM and its .sym file into <folder>.
  python3 <file> <test> [<arg>...]
    The file runs the test. The emulator is $RP6502_EMU, the ROMs are in
    $RP6502_ROMS, and each <arg> goes to the emulator.
"""

import ast
import os
import subprocess
import sys
from pathlib import Path

import rp6502

TESTS_DIR = Path(__file__).resolve().parents[1]
ADVENTURE = str(TESTS_DIR / "roms" / "adventure.rp6502")


def rom(name):
    """Returns the path of the ROM <name> in $RP6502_ROMS."""
    return str(Path(os.environ["RP6502_ROMS"]) / f"{name}.rp6502")


def save_dir():
    """Returns the folder behind SAVE:, which ctest gives with --save-dir."""
    return Path(sys.argv[sys.argv.index("--save-dir") + 1]).resolve()


def script(text, *words):
    """Runs <text>, an emulator script that the test file holds as a string.
    One blank line for each line of the file above the string comes before
    the script, so the emulator reports a failure at its line in the file."""
    tree = ast.parse(Path(sys.modules["__main__"].__file__).read_text("utf-8"))
    line = next(n.lineno for n in ast.walk(tree)
                if isinstance(n, ast.Constant) and n.value == text)
    result = subprocess.run(rp6502.Emu.command("--script", "-", *words),
                            input="\n" * (line - 1) + text, encoding="utf-8")
    if result.returncode:
        sys.exit(result.returncode)


def main(names):
    """Writes the ROMs of the test file or runs one of its tests."""
    if sys.argv[1:2] == ["--emit"]:
        folder = Path(sys.argv[2])
        for name, make in names.get("ROMS", {}).items():
            make().write(folder / f"{name}.rp6502", names)
        return
    names["TESTS"][sys.argv.pop(1)]()
