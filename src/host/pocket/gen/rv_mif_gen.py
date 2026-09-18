#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Quartus converts the lane files from rv_tcm_gen.py into four generated
# MIFs under db/ during mapping, and quartus_cdb --update_mif reads those
# MIFs rather than the lane files. New firmware therefore goes into a
# finished fit by rewriting the four db/ MIFs in place under their
# existing headers.

import sys
from pathlib import Path


def header(path):
    head = []
    with path.open() as f:
        for line in f:
            head.append(line.rstrip("\n"))
            if head[-1] == "CONTENT BEGIN":
                return head
    return None


def lane_mif(db, lane, words):
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
