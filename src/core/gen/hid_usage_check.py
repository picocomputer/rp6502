#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# core/hid/usage.h against the vendored TinyUSB header.
#
# core/hid/keyboard.c is compiled for machines that mostly have no USB, so the
# usage table it reads is the machine's own -- learning that Escape is 0x29
# should not cost a Pocket a USB stack. But the RIA does have USB and its
# drivers speak TinyUSB's spelling of the same specification, and two
# spellings of one specification is exactly the arrangement that drifts.
# So every constant core/hid/usage.h defines is compared against the
# vendor's here, on the one build that has both.
#
# Only names usage.h defines are checked: it carries what keyboard.c uses, not
# the whole usage table.

import argparse
import re
from pathlib import Path

# TinyUSB writes the bit flags as an enum with TU_BIT(n).
TU_BIT = re.compile(r"TU_BIT\((\d+)\)")


def evaluate(text):
    text = text.strip()
    m = TU_BIT.fullmatch(text)
    if m:
        return 1 << int(m.group(1))
    return int(text, 0)


def defines(src):
    """Every #define and enumerator with a constant value."""
    out = {}
    for m in re.finditer(r"^\s*#\s*define\s+([A-Z][A-Z0-9_]*)\s+"
                         r"(0[xX][0-9A-Fa-f]+|\d+)\s*(?://.*)?$",
                         src, re.M):
        out[m.group(1)] = evaluate(m.group(2))
    for m in re.finditer(r"\b([A-Z][A-Z0-9_]*)\s*=\s*"
                         r"(TU_BIT\(\d+\)|0[xX][0-9A-Fa-f]+|\d+)",
                         src):
        out.setdefault(m.group(1), evaluate(m.group(2)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--usage", required=True)
    ap.add_argument("--vendor", required=True)
    a = ap.parse_args()

    usage = defines(Path(a.usage).read_text(encoding="utf-8"))
    vendor = defines(Path(a.vendor).read_text(encoding="utf-8"))
    if not usage:
        raise SystemExit("hid_usage_check: core/hid/usage.h defines nothing")

    bad = []
    for name, value in sorted(usage.items()):
        if name not in vendor:
            bad.append(f"{name} is not in the vendored header")
        elif vendor[name] != value:
            bad.append(f"{name} is {value:#04x} here, {vendor[name]:#04x} there")
    if bad:
        raise SystemExit("hid_usage_check:\n  " + "\n  ".join(bad))
    print(f"hid usage: {len(usage)} constants match TinyUSB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
