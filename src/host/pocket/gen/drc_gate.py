#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The fitter can duplicate a register, and the Design Assistant report can
# list the copy, named with a ~DUPLICATE suffix, as a separate structure,
# so pocket_file_id[7] and pocket_file_id[7]~DUPLICATE appear as two Rule
# D101 structures for one clock-domain crossing.

import re
import sys
from collections import defaultdict
from pathlib import Path

# T101 and T102 are information-only rules that list nodes with high
# fan-out, so their findings are not defects.
IGNORED = ("T101", "T102")

# In the report, a finding's first row names the rule and may hold a node
# in its second cell. The finding's other nodes follow on rows indented two
# spaces, one node in the second cell of each. The table of enabled rules
# uses the same first row with "On" in the second cell.
RULE_ROW = re.compile(r"^;\s*Rule\s+(\w+):\s*(.*?)\s*;\s*([^;]*?)\s*;")
NODE_ROW = re.compile(r"^;\s{2,}[^;]*;\s*([^;]*?)\s*;")


def parse_summary(text: str) -> dict[str, int]:
    found: dict[str, int] = {}
    for rule, count in re.findall(
            r"^;\s*-\s*Rule\s+(\w+)\s*;\s*(\d+)\s*;", text, re.M):
        if rule not in IGNORED:
            found[rule] = int(count)
    return found


def fold(node: str) -> str:
    return node.replace("~DUPLICATE", "")


def parse_findings(text: str) -> dict[str, int]:
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
    found = {r: named.get(r, n) for r, n in structures.items()}
    allowed = parse_baseline(baseline.read_text()) if baseline.exists() else {}

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
