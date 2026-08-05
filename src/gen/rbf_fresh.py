#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Refuse to package a bitstream older than the fit it claims to be.
#
# The package target copies the reversed bitstream and does not depend on the
# bitstream target, deliberately: assembling a card should not cost a
# refit. The cost of that is a build where the fit ran, a gate after it
# failed, rbf_r_gen never ran, and the copy took the previous one
# without a word. That happened — a bitstream from twenty minutes and
# one RTL change earlier was packaged and very nearly tested, which
# would have measured the wrong design and sent the search somewhere it
# did not belong.
#
# The assembler's own output is the witness: if output_files/*.rbf is
# newer than the .rbf_r beside it, the reverser did not run for this
# fit.

import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: rbf_fresh.py <rp6502.rbf> <rp6502.bin>",
              file=sys.stderr)
        return 2
    rbf, rbf_r = Path(sys.argv[1]), Path(sys.argv[2])
    if not rbf.exists():
        print(f"rbf_fresh: {rbf} missing — run `bitstream` first",
              file=sys.stderr)
        return 1
    if not rbf_r.exists():
        print(f"rbf_fresh: {rbf_r} missing — run `bitstream` first",
              file=sys.stderr)
        return 1
    if rbf_r.stat().st_mtime < rbf.stat().st_mtime:
        print(f"rbf_fresh: {rbf_r.name} is older than {rbf.name} — the last"
              " fit did not finish its gates, so this would package the"
              " bitstream before it", file=sys.stderr)
        return 1
    print("rbf_fresh: the bitstream is the one this fit made")
    return 0


if __name__ == "__main__":
    sys.exit(main())
