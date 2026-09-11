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
# What is counted is names, not the structures the report numbers. The
# fitter copies a register when placement wants one, and the report
# lists the copy as a structure of its own -- pocket_file_id[7] and
# pocket_file_id[7]~DUPLICATE are one crossing counted twice -- so the
# structure count moved with placement and a change in one module
# failed the gate for a copy the fitter made in another. A finding here
# is the set of nodes the report lists for it with those copies folded
# into the bits they copy, and a rule's number is how many different
# findings that leaves. The baseline is in the same units.
#
# Lower the baseline when a fix lands. The file is the record of what is
# known-bad and why, so an entry that no longer fires should go.

import re
import sys
from collections import defaultdict
from pathlib import Path

# Information-only rules count fan-out and are not defects.
IGNORED = ("T101", "T102")

# A finding's first row names the rule; a structure's own nodes follow on
# rows indented two spaces, one node in the second cell of each. The
# enable table has the same first row with "On" where a node would be.
RULE_ROW = re.compile(r"^;\s*Rule\s+(\w+):\s*(.*?)\s*;\s*([^;]*?)\s*;")
NODE_ROW = re.compile(r"^;\s{2,}[^;]*;\s*([^;]*?)\s*;")


def parse_summary(text: str) -> dict[str, int]:
    """Rule id -> structure count, from the Design Assistant Summary."""
    found: dict[str, int] = {}
    for rule, count in re.findall(
            r"^;\s*-\s*Rule\s+(\w+)\s*;\s*(\d+)\s*;", text, re.M):
        if rule not in IGNORED:
            found[rule] = int(count)
    return found


def fold(node: str) -> str:
    return node.replace("~DUPLICATE", "")


def parse_findings(text: str) -> dict[str, int]:
    """Rule id -> how many different findings it lists, by name."""
    findings: dict[str, set[frozenset[str]]] = defaultdict(set)
    rule = None
    names: set[str] | None = None

    def close() -> None:
        if rule and names is not None:
            findings[rule].add(frozenset(names))

    for line in text.splitlines():
        m = RULE_ROW.match(line)
        if m:
            close()
            rule, desc, node = m.groups()
            if node == "On" or (not node and "- Structure" not in desc):
                rule, names = None, None
                continue
            names = {fold(node)} if node else set()
            continue
        m = NODE_ROW.match(line)
        if m and names is not None:
            if m.group(1):
                names.add(fold(m.group(1)))
            continue
        close()
        rule, names = None, None
    close()
    return {r: len(s) for r, s in findings.items() if r not in IGNORED}


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

    structures = parse_summary(text)
    named = parse_findings(text)
    # A rule the summary counts but the detail does not list is scored on
    # the count, which is all there is to read.
    found = {r: named.get(r, n) for r, n in structures.items()}
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
        print(f"  Rule {rule:<5} {n:5d}   structures {structures[rule]:5d}"
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
