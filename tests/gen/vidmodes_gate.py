#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import re
import sys
from pathlib import Path


def names(manifest):
    out = []
    for line in Path(manifest).read_text().splitlines():
        if line.strip():
            out.append(line.split()[0])
    return out


def read(suite, known):
    literals = re.findall(r'"((?:[^"\\\n]|\\.)*)"', Path(suite).read_text())
    seen = set()
    for lit in literals:
        for name in known:
            if re.search(r"(?<![0-9A-Za-z_])" + re.escape(name) +
                         r"(?![0-9A-Za-z_])", lit):
                seen.add(name)
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--suite", action="append", required=True)
    a = ap.parse_args()

    known = names(a.manifest)
    if not known:
        print("vidmodes_gate: the manifest is empty", file=sys.stderr)
        return 1

    seen = set()
    per = []
    for suite in a.suite:
        mine = read(suite, known)
        per.append(f"{Path(suite).name} {len(mine)}")
        seen |= mine
    missing = sorted(set(known) - seen)
    if missing:
        print(f"{len(known) - len(missing)} of {len(known)} fixtures are read "
              f"by a suite, missing: {', '.join(missing)}", file=sys.stderr)
        return 1
    print(f"vidmodes: {len(known)} fixtures, all read — {', '.join(per)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
