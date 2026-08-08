#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Fail the build on negative slack. A bitstream that misses timing does
# not announce itself: Quartus writes it out, the assembler is happy,
# and the part runs until it is warm or unlucky. The one that nearly
# shipped from this tree missed by 1.256 ns in a single module and was
# caught by somebody looking, which is not a process.
#
# The number that matters is the worst across every corner, not the one
# for the model the report happens to print first. This design has been
# worse at 0 C than at 85 C, so reading one corner would have passed it.

import re
import sys
from pathlib import Path

# Recovery and removal are reported per corner too, and a design can
# close setup while failing them; all five are the same question.
COLUMNS = ("Setup", "Hold", "Recovery", "Removal", "Minimum Pulse Width")


# The paths report is sta_paths.tcl's, written beside the signoff
# numbers so a failure can say which register pair it was. A CI fit is
# gone the moment the runner is; a slack with no name cannot be fixed.
def print_paths(report: Path) -> None:
    if not report.exists():
        return
    keep = re.compile(r"^;\s*-?\d|^; Slack\b")
    lines = [ln for ln in report.read_text(errors="replace").splitlines()
             if keep.match(ln)]
    if lines:
        print("\nthe worst paths of this fit, from "
              f"{report.name}:", file=sys.stderr)
        for ln in lines:
            print(ln, file=sys.stderr)


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print("usage: sta_gate.py <project.sta.rpt> [project.paths.rpt]",
              file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    if not path.exists():
        print(f"sta_gate: {path} missing — did the fit run?", file=sys.stderr)
        return 1
    text = path.read_text(errors="replace")

    row = re.search(r"^; Worst-case Slack\s+;(.+?);\s*$", text, re.M)
    if not row:
        print("sta_gate: no Worst-case Slack row; the report is not a"
              " timing report or its format moved", file=sys.stderr)
        return 1
    values = [c.strip() for c in row.group(1).split(";") if c.strip()]

    worst = []
    for name, cell in zip(COLUMNS, values):
        try:
            worst.append((name, float(cell)))
        except ValueError:
            print(f"sta_gate: {name} reads {cell!r}, not a number",
                  file=sys.stderr)
            return 1
    if not worst:
        print("sta_gate: the slack row was empty", file=sys.stderr)
        return 1

    bad = [(n, v) for n, v in worst if v < 0]
    for name, value in worst:
        print(f"  {name:<21} {value:+8.3f} ns{'   FAILS' if value < 0 else ''}")
    if bad:
        print("\nsta_gate: " + ", ".join(f"{n} {v:+.3f}" for n, v in bad)
              + " — the bitstream is not closed", file=sys.stderr)
        if len(sys.argv) == 3:
            print_paths(Path(sys.argv[2]))
        return 1
    print("\nsta_gate: closed on every corner")
    return 0


if __name__ == "__main__":
    sys.exit(main())
