#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The Pocket's HID constant shim against the vendored TinyUSB header.
#
# ria/hid/kbd.c is compiled for two machines. One has USB and gets
# TinyUSB's class/hid/hid.h; the other has an APF bus and gets
# src/rtl/sw/shim/class/hid/hid.h, which exists because learning that
# Escape is 0x29 should not cost a USB stack. Two spellings of one
# specification is exactly the arrangement that drifts, so every
# constant the shim defines is compared against the vendor's here.
#
# Only names the shim defines are checked: it carries what kbd.c uses,
# not the whole usage table.

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
    ap.add_argument("--shim", required=True)
    ap.add_argument("--vendor", required=True)
    a = ap.parse_args()

    shim = defines(Path(a.shim).read_text(encoding="utf-8"))
    vendor = defines(Path(a.vendor).read_text(encoding="utf-8"))
    if not shim:
        raise SystemExit("hid_shim_check: the shim defines nothing")

    bad = []
    for name, value in sorted(shim.items()):
        if name not in vendor:
            bad.append(f"{name} is not in the vendored header")
        elif vendor[name] != value:
            bad.append(f"{name} is {value:#04x} here, {vendor[name]:#04x} there")
    if bad:
        raise SystemExit("hid_shim_check:\n  " + "\n  ".join(bad))
    print(f"hid shim: {len(shim)} constants match TinyUSB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
