#!/usr/bin/env python3
"""Fail when a timing constraint can no longer match anything.

Quartus does not treat an unmatched collection as an error. It prints
"Ignored ... argument <targets> is an empty collection" and fits the design
without the constraint, so a renamed module silently drops whatever
set_multicycle_path or set_false_path was protecting it and the report still
says timing closed. rp6502.sdc carries a comment about this having already
happened.

So: pull every literal name out of the .sdc filters and require each one to
still appear somewhere in the RTL. This is a substring test, not an
elaboration -- it cannot tell a filter that matches the wrong thing from one
that matches the right thing. It only answers the question Quartus will not:
is there anything here at all.
"""

import argparse
import re
import sys
from pathlib import Path

# get_clocks names clocks, which are created by the .sdc itself rather than
# declared in RTL, so it is not checkable this way and not checked.
FILTER = re.compile(r"get_(?:registers|pins|ports)\s*\{([^}]*)\}")

# A Quartus PLL tap: ic|pll|pll|general[2].gpll~PLL_OUTPUT_COUNTER|divclk.
# The megafunction's internals are not ours and not in any file we read.
VENDOR_TAP = re.compile(r"~|gpll|PLL_OUTPUT")

WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def code_only(text):
    """Comments do not hold a design up. A module named only in prose would
    otherwise keep a dead filter looking alive."""
    return COMMENT.sub(" ", text)


def fragments(filter_text):
    """The literal names a filter needs something to match."""
    # A tap into a megafunction is vendor structure end to end, including the
    # plain-looking last hop, so the whole filter is skipped rather than the
    # one segment carrying the tilde.
    if VENDOR_TAP.search(filter_text):
        return
    for segment in filter_text.split("|"):
        # entity:instance narrowing, [*] and [3:0] indices, and the wildcards
        # themselves are structure rather than name.
        segment = segment.split(":")[0]
        segment = re.sub(r"\[[^\]]*\]", "", segment)
        for word in WORD.findall(segment):
            # Two characters is a suffix convention (_s1, _t1), not a name.
            if len(word) > 2:
                yield word


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdc", action="append", required=True, type=Path)
    ap.add_argument("--rtl", action="append", default=[], type=Path)
    # The constraints also filter on vendored and board-top names, so the
    # haystack is wider than the machine's own source list.
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
