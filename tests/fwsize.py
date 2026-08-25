#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Whether a change moved any code, for a firmware image.
#
# Section totals cannot answer that: assert() embeds __FILE__, so moving a file
# changes .rodata by the difference in path lengths and says nothing about
# .text. Function sizes can, but only after canonicalising -- LTO renames what
# it clones or partitions, and the N in .lto_priv.N counts the partitions
# referencing a symbol, which an include line can change without changing an
# instruction.
#
# So: strip the suffixes, fold the aliases that share an address, and compare
# the multiset of (name, size). A rename is reported apart from a resize.

import argparse
import re
import subprocess
import sys
from collections import defaultdict

# What LTO and the optimizers append. .N is a counter, not an identity.
MANGLE = re.compile(
    r"\.(lto_priv|constprop|isra|part|cold|localalias|clone)\.[0-9]+$"
    r"|\.(lto_priv|constprop|isra|part|cold|localalias|clone)$")


def canon(name):
    prev = None
    while prev != name:
        prev = name
        name = MANGLE.sub("", name)
    return name


def functions(nm, elf):
    """(name, size) per distinct text address; aliases at one address fold."""
    out = subprocess.run([nm, "-S", "--defined-only", elf],
                         capture_output=True, text=True, check=True).stdout
    at = defaultdict(set)
    size = {}
    for line in out.splitlines():
        f = line.split()
        # addr size type name -- symbols without a size have only 3 fields
        if len(f) != 4 or f[2] not in ("t", "T"):
            continue
        addr, sz, name = f[0], int(f[1], 16), canon(f[3])
        at[addr].add(name)
        size[addr] = sz
    tally = defaultdict(int)
    for addr, names in at.items():
        # One address, several spellings: pick one deterministically so the
        # count is of code, not of how LTO happened to name it this time.
        tally[(min(names), size[addr])] += 1
    return tally


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--nm", required=True, help="the target's nm")
    ap.add_argument("before")
    ap.add_argument("after")
    a = ap.parse_args()

    b, n = functions(a.nm, a.before), functions(a.nm, a.after)
    if b == n:
        print(f"unchanged: {sum(b.values())} functions, "
              f"{sum(sz * c for (_, sz), c in b.items())} bytes of text")
        return 0

    gone = {k: c for k, c in b.items() if n.get(k, 0) < c}
    came = {k: c for k, c in n.items() if b.get(k, 0) < c}
    # A name in both halves at two sizes is a resize; a name in one half only
    # is an add, a drop, or half of a rename.
    resized = {name for name, _ in gone} & {name for name, _ in came}

    for name in sorted(resized):
        was = [sz for (nm_, sz) in gone if nm_ == name]
        now = [sz for (nm_, sz) in came if nm_ == name]
        print(f"  resized  {name}: {was} -> {now}")
    for (name, sz), c in sorted(gone.items()):
        if name not in resized:
            print(f"  dropped  {name} ({sz} bytes){'' if c == 1 else f' x{c}'}")
    for (name, sz), c in sorted(came.items()):
        if name not in resized:
            print(f"  added    {name} ({sz} bytes){'' if c == 1 else f' x{c}'}")

    db = sum(sz * c for (_, sz), c in b.items())
    dn = sum(sz * c for (_, sz), c in n.items())
    print(f"CHANGED: text {db} -> {dn} ({dn - db:+d})")
    return 1


if __name__ == "__main__":
    sys.exit(main())
