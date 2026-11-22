#!/usr/bin/env python3
#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

"""Holds the RIA's console terminal against the Pico VGA's (xcheck.c).

  xcheck.py TEXT MODE0 fuzz SEED STEPS
  xcheck.py TEXT MODE0 file PATH SEED

TEXT and MODE0 are xcheck built with the text cells and with mode 0's.
Both are run on the same input, and every point's digest must be the same.
TEXT, the RIA, runs first and writes the selection's words it sends each
frame to a file, which MODE0, the VGA, takes them from. At the first point
that differs, both print all they hold there, and the rows that differ are
shown. A file that is not there is skipped, as vttest's stream is when the
walk did not give it to xterm."""

import difflib
import os
import subprocess
import sys
import tempfile

SKIP = 77


def run(build, words, args):
    out = subprocess.run([build, words] + args, check=True, stdout=subprocess.PIPE)
    return out.stdout.decode().splitlines()


def main():
    text, mode0, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    if args[0] == "file" and not os.path.exists(args[1]):
        print(f"xcheck: no {args[1]}, skipped")
        return SKIP
    with tempfile.TemporaryDirectory() as tmp:
        return check(text, mode0, os.path.join(tmp, "words"), args)


def check(text, mode0, words, args):
    a = run(text, words, args)
    b = run(mode0, words, args)
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            point = x.split()[0]
            print(f"xcheck: the text cells and mode 0's differ at point {point}")
            da = run(text, words, args + [point])
            db = run(mode0, words, args + [point])
            diff = list(difflib.unified_diff(db, da, "mode0", "text", lineterm="", n=1))
            print("\n".join(diff[:80]))
            return 1
    if len(a) != len(b) or not a:
        print(f"xcheck: {len(a)} points of the text cells, {len(b)} of mode 0's")
        return 1
    print(f"xcheck: {len(a)} points the same")
    return 0


if __name__ == "__main__":
    sys.exit(main())
