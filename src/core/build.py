#!/usr/bin/env python3
#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

"""The Python helpers that the scripts which build and test a Picocomputer
share. The SDK's tools are in tools/.

The build runs every script through this file, as build.cmake's
RP6502_PYTHON does:

    python -X utf8 -B src/core/build.py SCRIPT [ARGS...]

which puts this folder, tools/ and tests/lib/ on the import path, so a script
imports build, rp6502 and the test libraries without changing sys.path
itself. A generator is run by hand the same way.
"""

import argparse
import importlib.util
import os
import re
import runpy
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TOOLS = ROOT / "tools"
TEST_LIB = ROOT / "tests" / "lib"


# ---- generated files ------------------------------------------------------
#
# A generated file has the same bytes on every host: UTF-8, with a bare LF
# ending each line, which Python's text mode would otherwise turn into CRLF
# on Windows.

def open_text(path):
    """A generated text file, opened for writing."""
    return open(path, "w", encoding="utf-8", newline="\n")


def write_text(path, text):
    """Writes a generated text file."""
    with open_text(path) as f:
        f.write(text)


# ---- generators -----------------------------------------------------------
#
# The command line every generator takes, which gen.cmake drives:
#
#   --list       one line per file: "in PATH" for each file the generator
#                reads, PATH from the repo root, then "KIND NAME" for each
#                file it writes
#   --c DIR      writes the C headers and sources into DIR
#   --sv DIR     writes the SystemVerilog packages into DIR
#
# A generator passes its inputs and, for each kind, the name of each file
# with the function that writes it.

KINDS = ("c", "sv")


def generate(inputs=(), c=None, sv=None, extra=None):
    """Returns the parsed arguments, for a generator with options of its
    own, which extra adds to the parser."""
    outputs = {"c": c or {}, "sv": sv or {}}
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    for kind in KINDS:
        ap.add_argument(f"--{kind}", metavar="DIR")
    if extra:
        extra(ap)
    args = ap.parse_args()
    if args.list:
        for path in inputs:
            print("in", path)
        for kind in KINDS:
            for name in outputs[kind]:
                print(kind, name)
    for kind in KINDS:
        out = getattr(args, kind)
        if out:
            for name, write in outputs[kind].items():
                write(Path(out) / name)
    return args


# ---- vendored modules -----------------------------------------------------

def load_module(path, name):
    """A Python file that is not part of a package, imported once under name,
    with its own folder on the import path for the files beside it."""
    if name in sys.modules:
        return sys.modules[name]
    sys.path.insert(0, str(Path(path).parent))
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


# ---- toolchain tools ------------------------------------------------------

def run(*cmd, input=None, cwd=None):
    """Runs a tool to the end and returns what it printed. The input goes in
    and the output comes back whole, so no pipe can fill and stall it."""
    return subprocess.run(cmd, input=input, cwd=cwd, capture_output=True,
                          text=True, check=True).stdout


def split_location(loc):
    """A "file:line" from addr2line or a compiler, as (file, line). The line
    is after the last colon, as a Windows file starts with a drive letter and
    a colon of its own. Line is None when the tool printed ?."""
    m = re.match(r"(.*):(\d+|\?)(?: \(discriminator \d+\))?$", loc)
    if not m:
        return loc, None
    return m.group(1), None if m.group(2) == "?" else int(m.group(2))


def same_path(path):
    """A path in the one spelling every comparison uses: absolute, links
    resolved, and in the host's case and separators."""
    return os.path.normcase(os.path.realpath(path))


# ---- running a script -----------------------------------------------------

def main():
    if len(sys.argv) < 2:
        sys.exit("usage: build.py SCRIPT [ARGS...]")
    script = os.path.abspath(sys.argv[1])
    sys.argv = sys.argv[1:]
    # Every folder is named, as an embedded Python, such as the Pico VS Code
    # extension's, puts no script's folder on the path, and they go ahead of
    # the rest, which it starts with its standard library.
    sys.path[:0] = [os.path.dirname(script), str(HERE), str(TOOLS), str(TEST_LIB)]
    runpy.run_path(script, run_name="__main__")


if __name__ == "__main__":
    main()
