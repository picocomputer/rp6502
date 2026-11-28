#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import fnmatch
import re
import sys
from pathlib import Path

COLUMNS = ("Setup", "Hold", "Recovery", "Removal", "Minimum Pulse Width")

# Each analysis has a per-clock summary table for each corner the analyzer
# ran: "Slow 1100mV 85C Model Setup Summary" and so on, or a bare "Setup
# Summary" when the project turns multicorner analysis off. The worst slack
# of an analysis is the least over all of its tables. A multicorner report
# also has a Worst-case Slack row. Every analysis must have a table for each
# corner, and the tables must agree with that row, so a table whose header
# moved cannot drop out of the check unseen.
TABLE = re.compile(r"^; (?:(.*) Model )?(" + "|".join(COLUMNS) + r") Summary\s*;$")
ROW = re.compile(r"^; Worst-case Slack\s*;(.*);\s*$", re.M)

# Quartus fits a design without a constraint whose collection came up empty
# (332049) or whose filter matched nothing (332174), or without an SDC file
# it could not read (332012), and only warns.
DROPPED = re.compile(
    r"^\s*(?:Critical )?Warning \((?:332049|332174|332012)\):.*$", re.M)
# A filter Quartus ignored names its SDC file and the filter itself.
IGNORED_FILTER = re.compile(r"Ignored filter at ([^(\s]+)\(\d+\): (\S+) ")


def accepted(path: Path) -> set[tuple[str, str]]:
    """The "<sdc file> ; <filter>" lines of a host's list of filters that
    may match nothing."""
    out = set()
    for line in path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            sdc, _, flt = line.partition(";")
            out.add((sdc.strip(), flt.strip()))
    return out


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


# The machine's SDC applies its multicycles to whatever registers its
# wildcards match, so a register renamed into or out of one, or a host's
# instance whose name happens to end in cpu, changes a constraint without
# any warning. sta_paths.tcl writes the size of each collection to the
# names report, and each host keeps the sizes its fit should have.
def check_counts(names: Path, counts: Path) -> list[str]:
    found: dict[str, int] = {}
    for line in names.read_text(errors="replace").splitlines():
        kind, *cells = line.split(";")
        if kind == "collection":
            found[cells[1]] = int(cells[0])
    want: dict[str, int] = {}
    for line in counts.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            n, pattern = line.split(None, 1)
            want[pattern] = int(n)
    bad = []
    for pattern, n in found.items():
        if pattern not in want:
            bad.append(f"{{{pattern}}} matches {n} registers and"
                       f" {counts.name} has no count for it")
        elif n != want[pattern]:
            bad.append(f"{{{pattern}}} matches {n} registers, not"
                       f" {want[pattern]}")
    for pattern in want.keys() - found.keys():
        bad.append(f"{{{pattern}}} is in {counts.name} but in no SDC"
                   " collection")
    return bad


def corners(path: Path) -> list[tuple[str, str]]:
    """The "<clock glob> ; <corner>" lines of a host's list of clocks judged
    at one corner only."""
    out = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            clk, _, corner = line.partition(";")
            out.append((clk.strip(), corner.strip()))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("sta_rpt")
    ap.add_argument("paths_rpt", nargs="?",
                    help="sta_paths.tcl's worst paths, shown on a failure")
    ap.add_argument("--names", type=Path,
                    help="sta_paths.tcl's names report")
    ap.add_argument("--counts", type=Path,
                    help="the register count each SDC collection must match")
    ap.add_argument("--accepted", type=Path,
                    help="filters that may match nothing, with the reason")
    ap.add_argument("--corners", type=Path,
                    help="clocks judged at one corner only, with the reason")
    a = ap.parse_args()
    if bool(a.names) != bool(a.counts):
        ap.error("--names and --counts go together")
    for path in [Path(a.sta_rpt)] + ([a.names, a.counts] if a.names else []):
        if not path.exists():
            print(f"sta_gate: {path} missing — did the fit run?",
                  file=sys.stderr)
            return 1
    path = Path(a.sta_rpt)
    text = path.read_text(errors="replace")

    dropped = DROPPED.findall(text)
    if a.accepted:
        ok = accepted(a.accepted)
        dropped = [d for d in dropped
                   if (m := IGNORED_FILTER.search(d)) is None
                   or m.groups() not in ok]

    only = corners(a.corners) if a.corners else []
    slack: dict[str, list[float]] = {}
    every: dict[str, list[float]] = {}
    skipped: list[str] = []
    tables: dict[str, int] = {}
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = TABLE.match(line.rstrip())
        if not m:
            continue
        corner, name = m.group(1) or "", m.group(2)
        tables[name] = tables.get(name, 0) + 1
        for row in lines[i + 1:]:
            if not row.strip():
                break
            cells = [c.strip() for c in row.strip(";").split(";")]
            if not row.startswith(";") or len(cells) < 2 or \
                    cells[1] == "Slack":
                continue
            try:
                value = float(cells[1])
                every.setdefault(name, []).append(value)
                if any(fnmatch.fnmatchcase(cells[0], clk) and corner != c
                       for clk, c in only):
                    skipped.append(f"{corner} {name} {cells[0]} {value:+.3f}")
                    continue
                slack.setdefault(name, []).append(value)
            except ValueError:
                print(f"sta_gate: {name} of {cells[0]} reads {cells[1]!r},"
                      " not a number", file=sys.stderr)
                return 1
    if "Setup" not in every or "Hold" not in every:
        print("sta_gate: no Setup and Hold summary tables; the report is not"
              " a timing report or its format moved", file=sys.stderr)
        return 1
    if len(set(tables.values())) != 1:
        print("sta_gate: the corners' summary tables do not pair up ("
              + ", ".join(f"{n} {c}" for n, c in tables.items())
              + "); a table's format moved", file=sys.stderr)
        return 1
    worst = [(n, min(slack[n])) for n in COLUMNS if n in slack]

    row = ROW.search(text)
    if row:
        found = {n: min(every[n]) for n in COLUMNS if n in every}
        for name, cell in zip(COLUMNS, row.group(1).split(";")):
            cell = cell.strip()
            if cell != "N/A" and found.get(name) != float(cell):
                print(f"sta_gate: the Worst-case Slack row has {name} {cell}"
                      f" but the corner tables give {found.get(name)}; a"
                      " table's format moved", file=sys.stderr)
                return 1

    bad = [(n, v) for n, v in worst if v < 0]
    for line in skipped:
        if line.split()[-1].startswith("-"):
            print(f"  not judged at this corner: {line}")
    for name, value in worst:
        print(f"  {name:<21} {value:+8.3f} ns{'   FAILS' if value < 0 else ''}")
    if bad:
        print("\nsta_gate: " + ", ".join(f"{n} {v:+.3f}" for n, v in bad)
              + " — the bitstream is not closed", file=sys.stderr)
        if a.paths_rpt:
            print_paths(Path(a.paths_rpt))
    if dropped:
        print(file=sys.stderr)
        for line in dropped:
            print(line.strip(), file=sys.stderr)
        print(f"sta_gate: {len(dropped)} constraint(s) dropped — an SDC"
              " file or a filter Quartus could not apply", file=sys.stderr)
    moved = check_counts(a.names, a.counts) if a.names else []
    if moved:
        print(file=sys.stderr)
        for line in moved:
            print(f"sta_gate: {line}", file=sys.stderr)
        print(f"sta_gate: a constraint now covers other registers than"
              f" it did; check the SDC against the RTL, then set"
              f" {a.counts.name} to what the fit matches", file=sys.stderr)
    if bad or dropped or moved:
        return 1
    print("\nsta_gate: closed on " + ("every corner" + (
        ", the listed clocks at theirs" if only else "") if row else
                                      "the analysed corner"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
