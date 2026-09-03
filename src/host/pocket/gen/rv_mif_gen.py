#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The soft CPU's firmware, into a bitstream that has already been fitted.
#
# rv_tcm_gen.py writes the lane files the synthesizer reads. Quartus turns
# those into four MIFs of its own under db/ while mapping, and
# quartus_cdb --update_mif reads *those* back rather than the lane files
# they came from — so putting new firmware into a finished fit means
# rewriting them in place. Same header, same geometry, new contents; the
# assembler then makes a programming file out of the placement that is
# already there.
#
# Editing a tool's own database is only defensible because of what this
# particular file is. Memory contents place nothing and route nothing, so
# the fit these belong to is still the fit that comes out. The freshness
# gate in synth.cmake is what keeps that sentence true.

import sys
from pathlib import Path


def header(path):
    """Everything down to CONTENT BEGIN, or None if it never comes."""
    head = []
    with path.open() as f:
        for line in f:
            head.append(line.rstrip("\n"))
            if head[-1] == "CONTENT BEGIN":
                return head
    return None


def lane_mif(db, lane, words):
    """The MIF this fit is using for one TCM byte lane.

    Quartus leaves a superseded fit's MIFs behind, so the glob can match
    several hashes. Geometry settles most of it — a TCM of another size
    belongs to another design — and the newest wins whatever is left.
    """
    hits = []
    for path in sorted(db.glob(f"*.ram{lane}_soc_*.hdl.mif")):
        head = header(path)
        if head and "WIDTH=8;" in head and f"DEPTH={words};" in head:
            hits.append((path, head))
    if not hits:
        return None, None
    return max(hits, key=lambda h: h[0].stat().st_mtime)


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: rv_mif_gen.py <sw.bin> <db-dir> <words>",
              file=sys.stderr)
        return 2
    data = Path(sys.argv[1]).read_bytes()
    db = Path(sys.argv[2])
    words = int(sys.argv[3])
    size = len(data)
    if size > words * 4:
        print(f"rv_mif_gen: {size} bytes will not fit {words} words",
              file=sys.stderr)
        return 1
    data = data + bytes(words * 4 - size)
    for lane in range(4):
        path, head = lane_mif(db, lane, words)
        if path is None:
            print(f"rv_mif_gen: no {words}-word lane {lane} MIF in {db} —"
                  " this fit is not one `bitstream` made", file=sys.stderr)
            return 1
        # The two radices the map step has always chosen. Another pair is
        # not a format to guess at, it is a Quartus that changed its mind.
        if "ADDRESS_RADIX=UNS;" not in head or "DATA_RADIX=BIN;" not in head:
            print(f"rv_mif_gen: {path.name} is not UNS/BIN", file=sys.stderr)
            return 1
        body = "".join(f"\t{addr} :\t{data[addr * 4 + lane]:08b};\n"
                       for addr in range(words - 1, -1, -1))
        path.write_text("\n".join(head) + "\n" + body + "END;\n")
    print(f"rv_mif_gen: {size} bytes into four lane MIFs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
