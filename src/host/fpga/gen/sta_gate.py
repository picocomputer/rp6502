#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

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
TABLE = re.compile(r"^; (?:.* Model )?(" + "|".join(COLUMNS) + r") Summary\s*;$")
ROW = re.compile(r"^; Worst-case Slack\s*;(.*);\s*$", re.M)

# Quartus fits a design without a constraint whose collection came up empty
# (332049) or whose filter matched nothing (332174), or without an SDC file
# it could not read (332012), and only warns.
DROPPED = re.compile(
    r"^\s*(?:Critical )?Warning \((?:332049|332174|332012)\):.*$", re.M)


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

    dropped = DROPPED.findall(text)

    slack: dict[str, list[float]] = {}
    tables: dict[str, int] = {}
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = TABLE.match(line.rstrip())
        if not m:
            continue
        name = m.group(1)
        tables[name] = tables.get(name, 0) + 1
        for row in lines[i + 1:]:
            if not row.strip():
                break
            cells = [c.strip() for c in row.strip(";").split(";")]
            if not row.startswith(";") or len(cells) < 2 or \
                    cells[1] == "Slack":
                continue
            try:
                slack.setdefault(name, []).append(float(cells[1]))
            except ValueError:
                print(f"sta_gate: {name} of {cells[0]} reads {cells[1]!r},"
                      " not a number", file=sys.stderr)
                return 1
    if "Setup" not in slack or "Hold" not in slack:
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
        found = dict(worst)
        for name, cell in zip(COLUMNS, row.group(1).split(";")):
            cell = cell.strip()
            if cell != "N/A" and found.get(name) != float(cell):
                print(f"sta_gate: the Worst-case Slack row has {name} {cell}"
                      f" but the corner tables give {found.get(name)}; a"
                      " table's format moved", file=sys.stderr)
                return 1

    bad = [(n, v) for n, v in worst if v < 0]
    for name, value in worst:
        print(f"  {name:<21} {value:+8.3f} ns{'   FAILS' if value < 0 else ''}")
    if bad:
        print("\nsta_gate: " + ", ".join(f"{n} {v:+.3f}" for n, v in bad)
              + " — the bitstream is not closed", file=sys.stderr)
        if len(sys.argv) == 3:
            print_paths(Path(sys.argv[2]))
    if dropped:
        print(file=sys.stderr)
        for line in dropped:
            print(line.strip(), file=sys.stderr)
        print(f"sta_gate: {len(dropped)} constraint(s) dropped — an SDC"
              " file or a filter Quartus could not apply", file=sys.stderr)
    if bad or dropped:
        return 1
    print("\nsta_gate: closed on " + ("every corner" if row else
                                      "the analysed corner"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
