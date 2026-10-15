#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""rp6502_rom writes the ROMs and runs the tests of a test file.

A test file names its ROMs in ROMS, a dict of ROM name to a function that
returns an Asm or a Rom. It names its tests in TESTS, a dict of test name to
a function. It ends with main(globals()).

  python3 <file> --emit <folder>
    The file writes each ROM and its .sym file into <folder>.
  python3 <file> <test> [<arg>...]
    The file runs the test. The emulator is $RP6502_EMU, the ROMs are in
    $RP6502_ROMS, and each <arg> goes to the emulator.
"""

import ast
import importlib.util
import os
import subprocess
import sys
from pathlib import Path

import rp6502_asm
from rp6502_asm import ORG, Asm

TESTS_DIR = Path(__file__).resolve().parents[1]
ADVENTURE = str(TESTS_DIR / "roms" / "adventure.rp6502")


def tool():
    """Loads tools/rp6502.py by its path."""
    path = TESTS_DIR.parent / "tools" / "rp6502.py"
    spec = importlib.util.spec_from_file_location("rp6502_tool", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    cache, sys.dont_write_bytecode = sys.dont_write_bytecode, True
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.dont_write_bytecode = cache
    return mod


rp6502 = tool()
ScriptError = rp6502.ScriptError


class Rom(rp6502.ROM):
    labels = {}

    def record(self, addr, data):
        self.add_binary_data(bytes(data), addr)
        return self

    def reset(self, org=ORG):
        self.add_reset_vector(org)
        return self

    def program(self, prog, org=ORG):
        if isinstance(prog, Asm):
            body = prog.code()
            self.labels = dict(prog._sym)
        else:
            body = bytes(prog)
        return self.record(org, body).reset(org)

    def write(self, path, names=None):
        """Writes the ROM and, beside it, its .sym file: the labels of the
        program and the whole numbers of rp6502_asm and of names."""
        size = super().write(path)
        pairs = list(self.labels.items())
        for table in (vars(rp6502_asm), names or {}):
            pairs += [(k, v) for k, v in table.items() if type(v) is int]
        rp6502.Symbols.write(os.path.splitext(str(path))[0] + ".sym", pairs)
        return size


def image(prog, org=ORG):
    return Rom().program(prog, org)


def rom(name):
    """Returns the path of the ROM <name> in $RP6502_ROMS."""
    return str(Path(os.environ["RP6502_ROMS"]) / f"{name}.rp6502")


def save_dir():
    """Returns the folder behind SAVE:, which ctest gives with --save-dir."""
    return Path(sys.argv[sys.argv.index("--save-dir") + 1]).resolve()


def emu(*words):
    """Returns rp6502.Emu, which sends script commands to the emulator."""
    return rp6502.Emu(*words)


def command(*words):
    """Returns the command line of the emulator: <words> and the arguments of
    the test. The words after -- come last, as the arguments of the ROM."""
    split = words.index("--") if "--" in words else len(words)
    return [os.environ["RP6502_EMU"], *words[:split], *sys.argv[1:],
            *words[split:]]


def script(text, *words):
    """Runs <text>, an emulator script that the test file holds as a string.
    One blank line for each line of the file above the string comes before
    the script, so the emulator reports a failure at its line in the file."""
    tree = ast.parse(Path(sys.modules["__main__"].__file__).read_text("utf-8"))
    line = next(n.lineno for n in ast.walk(tree)
                if isinstance(n, ast.Constant) and n.value == text)
    result = subprocess.run(command("--script", "-", *words),
                            input="\n" * (line - 1) + text, encoding="utf-8")
    if result.returncode:
        sys.exit(result.returncode)


def main(names):
    """Writes the ROMs of the test file or runs one of its tests."""
    roms, tests = names.get("ROMS", {}), names.get("TESTS", {})
    if sys.argv[1:2] == ["--emit"]:
        folder = Path(sys.argv[2])
        for name, make in roms.items():
            made = make()
            if isinstance(made, Asm):
                made = image(made)
            made.write(folder / f"{name}.rp6502", names)
        return
    test = tests[sys.argv.pop(1)]
    sys.exit(test() or 0)
