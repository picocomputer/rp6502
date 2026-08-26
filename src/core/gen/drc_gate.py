#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Fail the build when the Design Assistant finds something new. Timing
# analysis answers one question — does every path make its clock — and a
# design can pass it while an unsynchronised reset releases eight
# thousand flops at whatever moment the routing decides, or a command
# opcode crosses clock domains a bit at a time. Neither of those is a
# path that misses; both are bitstreams that work on one fit and not the
# next, which is the shape of failure this tree has now paid for three
# times.
#
# Nothing here was ever run. quartus_drc ships with the tools, costs
# seconds, and had 638 High findings waiting the first time it was
# asked.
#
# A baseline rather than a threshold, because the existing findings are
# not all bugs: gray-coded pointers are flagged by D102 because the tool
# cannot know the encoding makes misalignment safe, and D101 counts the
# clk_sys/clk_rv seam that the SDC declares synchronous on purpose. What
# matters is that the set does not grow. A rule that climbs by one is a
# new crossing somebody added without a synchroniser, and that is worth
# a failed build.
#
# Lower the baseline when a fix lands. The file is the record of what is
# known-bad and why, so an entry that no longer fires should go.

import re
import sys
from pathlib import Path

# Information-only rules count fan-out and are not defects.
IGNORED = ("T101", "T102")


def parse_report(text: str) -> dict[str, int]:
    """Rule id -> violation count, from the Design Assistant Summary."""
    found: dict[str, int] = {}
    for rule, count in re.findall(
            r"^;\s*-\s*Rule\s+(\w+)\s*;\s*(\d+)\s*;", text, re.M):
        if rule not in IGNORED:
            found[rule] = int(count)
    return found


def parse_baseline(text: str) -> dict[str, int]:
    allowed: dict[str, int] = {}
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            rule, count = line.split()
            allowed[rule] = int(count)
    return allowed


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: drc_gate.py <project.drc.rpt> <baseline>",
              file=sys.stderr)
        return 2
    report, baseline = Path(sys.argv[1]), Path(sys.argv[2])
    if not report.exists():
        print(f"drc_gate: {report} missing — did quartus_drc run?",
              file=sys.stderr)
        return 1
    text = report.read_text(errors="replace")
    if "Design Assistant Summary" not in text:
        print("drc_gate: no Design Assistant Summary; the report is not a"
              " rule check or its format moved", file=sys.stderr)
        return 1

    found = parse_report(text)
    allowed = parse_baseline(baseline.read_text()) if baseline.exists() else {}

    # A Critical finding is never baselined. The tool reserves that
    # severity for things that do not work rather than things that might.
    grew = [(r, n, allowed.get(r, 0)) for r, n in sorted(found.items())
            if n > allowed.get(r, 0)]
    shrank = [(r, n, allowed[r]) for r, n in sorted(found.items())
              if r in allowed and n < allowed[r]]
    gone = sorted(set(allowed) - set(found))

    for rule, n in sorted(found.items()):
        was = allowed.get(rule)
        mark = "  NEW" if n > (was or 0) else ""
        print(f"  Rule {rule:<5} {n:5d}"
              + (f"   baseline {was}" if was is not None else "")
              + mark)

    for rule, n, was in shrank:
        print(f"drc_gate: Rule {rule} fell {was} -> {n} —"
              " lower the baseline", file=sys.stderr)
    for rule in gone:
        print(f"drc_gate: Rule {rule} no longer fires — drop it from the"
              " baseline", file=sys.stderr)

    if grew:
        print("\ndrc_gate: " + ", ".join(
            f"Rule {r} {was} -> {n}" for r, n, was in grew)
            + " — a new violation entered the design", file=sys.stderr)
        return 1
    print("\ndrc_gate: no new violations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
