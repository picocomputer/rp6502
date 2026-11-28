#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# assignment_gate.py <map.rpt> <fit.rpt> <names.rpt> [--ignored FILE]
#
# Quartus compiles on when an assignment names nothing, and how much it
# says depends on who reads the assignment:
#
# - an assignment to an entity the design does not have: warning 20013 in
#   synthesis and the fitter, with a 20014 for each assignment;
# - one the fitter cannot apply: warning 171167 or 176250 and a row in the
#   fitter's Ignored Assignments panel;
# - one that only synthesis reads, such as AUTO_SHIFT_REGISTER_RECOGNITION,
#   on an instance path that is not in the design: nothing at all.
#
# So this gate reads the warnings and the panel, and for the silent case
# the names report sta_paths.tcl writes, which counts what the netlist
# matches for each instance assignment's target. A count of 0 fails. The
# file given as --ignored lists the assignments a host cannot avoid, one
# "<name> ; <target>" a line, the name as the panel shows it or as the
# assignment spells it.

import argparse
import re
import sys
from pathlib import Path

ENTITY = re.compile(r"^\s*(?:Critical )?Warning \((?:20013|20014)\):.*$",
                    re.M)
PANEL_WARNING = re.compile(r"^(?:Critical )?Warning \((?:171167|176250)\):",
                           re.M)

# These name no node of the netlist: the top partition, which Quartus calls
# "|", and the HPS's own peripherals.
NOT_NODES = ("PARTITION_HIERARCHY", "HPS_LOCATION")


def panel(text: str) -> list[tuple[str, str]] | None:
    lines = text.splitlines()
    try:
        start = next(i for i, ln in enumerate(lines)
                     if re.match(r"^; Ignored Assignments\s*;$", ln))
    except StopIteration:
        return None
    rows = []
    for ln in lines[start + 4:]:
        if not ln.startswith(";"):
            break
        cells = [c.strip() for c in ln.strip(";").split(";")]
        name, _, src, dst = cells[:4]
        rows.append((name, " -> ".join(t for t in (src, dst) if t)))
    return rows


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("map_rpt", type=Path)
    ap.add_argument("fit_rpt", type=Path)
    ap.add_argument("names_rpt", type=Path)
    ap.add_argument("--ignored", type=Path,
                    help="the assignments this host may leave unapplied")
    a = ap.parse_args()

    for path in (a.map_rpt, a.fit_rpt, a.names_rpt):
        if not path.exists():
            print(f"assignment_gate: {path} missing — did the fit run?",
                  file=sys.stderr)
            return 1

    allowed: set[tuple[str, str]] = set()
    if a.ignored:
        for line in a.ignored.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                name, target = (c.strip() for c in line.split(";", 1))
                allowed.add((name, target))

    entity = []
    for path in (a.map_rpt, a.fit_rpt):
        entity += [ln.strip() for ln in
                   ENTITY.findall(path.read_text(errors="replace"))]

    fit = a.fit_rpt.read_text(errors="replace")
    found = panel(fit)
    if found is None:
        if PANEL_WARNING.search(fit):
            print("assignment_gate: the fitter ignored assignments but its"
                  " Ignored Assignments panel is missing; its format moved",
                  file=sys.stderr)
            return 1
        found = []

    for line in a.names_rpt.read_text(errors="replace").splitlines():
        cells = line.split(";", 3)
        if cells[0] == "assignment" and cells[1] == "0" and \
                cells[2] not in NOT_NODES:
            found.append((cells[2], cells[3]))

    seen = set(found)
    bad = sorted(seen - allowed)
    for name, target in sorted(seen & allowed):
        print(f"  ignored, as expected: {name} ; {target}")
    for name, target in sorted(allowed - seen):
        print(f"  note: {name} ; {target} is applied now; drop it from"
              f" {a.ignored.name} unless another Quartus still ignores it")
    for line in entity:
        print(line, file=sys.stderr)
    for name, target in bad:
        print(f"assignment_gate: {name} ; {target} matches nothing or was"
              " ignored", file=sys.stderr)
    if entity or bad:
        print("\nassignment_gate: an assignment does not reach the design"
              " it was written for", file=sys.stderr)
        return 1
    print("assignment_gate: every assignment reaches the design")
    return 0


if __name__ == "__main__":
    sys.exit(main())
