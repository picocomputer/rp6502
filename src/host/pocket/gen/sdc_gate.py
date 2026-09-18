#!/usr/bin/env python3
"""Quartus does not treat an unmatched collection as an error. It prints a
warning such as "Ignored set_multicycle_path at ...: Argument <to> is an empty
collection" and fits the design without that constraint, so a constraint
whose filter names a renamed module is dropped without failing the build.
"""

import argparse
import re
import sys
from pathlib import Path

FILTER = re.compile(r"get_(?:registers|pins|ports)\s*\{([^}]*)\}")

# A tap into the Quartus PLL megafunction looks like
# ic|pll|pll|general[2].gpll~PLL_OUTPUT_COUNTER|divclk, and its internal
# names, such as gpll, PLL_OUTPUT_COUNTER and divclk, appear in no RTL that
# the gate reads.
VENDOR_TAP = re.compile(r"~|gpll|PLL_OUTPUT")

WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def code_only(text):
    return COMMENT.sub(" ", text)


def fragments(filter_text):
    if VENDOR_TAP.search(filter_text):
        return
    for segment in filter_text.split("|"):
        segment = segment.split(":")[0]
        segment = re.sub(r"\[[^\]]*\]", "", segment)
        for word in WORD.findall(segment):
            if len(word) > 2:
                yield word


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdc", action="append", required=True, type=Path)
    ap.add_argument("--rtl", action="append", default=[], type=Path)
    ap.add_argument("--rtl-dir", action="append", default=[], type=Path)
    args = ap.parse_args()

    sources = list(args.rtl)
    for d in args.rtl_dir:
        sources += sorted(d.rglob("*.sv")) + sorted(d.rglob("*.v"))

    haystack = []
    for path in sources:
        if path.exists():
            haystack.append(code_only(path.read_text(errors="replace")))
    if not haystack:
        sys.exit("sdc_gate: no RTL was readable")
    haystack = "\n".join(haystack)

    dead = []
    for sdc in args.sdc:
        if not sdc.exists():
            sys.exit(f"sdc_gate: {sdc} does not exist")
        for lineno, line in enumerate(sdc.read_text().splitlines(), 1):
            if line.lstrip().startswith("#"):
                continue
            for match in FILTER.finditer(line):
                for name in fragments(match.group(1)):
                    if name not in haystack:
                        dead.append((sdc, lineno, name, match.group(0)))

    for sdc, lineno, name, text in dead:
        print(f"{sdc}:{lineno}: '{name}' matches nothing in the RTL: {text}")
    if dead:
        print(f"\n{len(dead)} constraint(s) would be silently dropped by Quartus.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
