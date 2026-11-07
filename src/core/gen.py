# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The command line every generator takes, which core/gen.cmake drives:
#
#   --list       one line per file: "in PATH" for each file the generator
#                reads, PATH from the repo root, then "KIND NAME" for each
#                file it writes
#   --c DIR      writes the C headers and sources into DIR
#   --sv DIR     writes the SystemVerilog packages into DIR
#   --bin DIR    writes the images the Pocket loads into DIR
#
# A generator passes its inputs and, for each kind, the name of each file
# with the function that writes it.

import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KINDS = ("c", "sv", "bin")


def main(inputs=(), c=None, sv=None, bin=None, extra=None):
    """Returns the parsed arguments, for a generator with options of its
    own, which extra adds to the parser."""
    outputs = {"c": c or {}, "sv": sv or {}, "bin": bin or {}}
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
