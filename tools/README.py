#!/usr/bin/env python3
#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
# SPDX-License-Identifier: Unlicense

"""A new project starts with a small tools/rp6502.cmake that fetches SHA256SUMS
from this directory and then every file listed in it, in the listed order.
Fetching rp6502.cmake replaces that small fetcher with the full one, and a fetch
stops at its first failure, so rp6502.cmake is listed last: a project that has
the full rp6502.cmake has every tool listed before it. Every download is checked
against its hash in SHA256SUMS, so SHA256SUMS has to be regenerated with this
script after any tool changes.

A download is the file as Git stores it. A Git that checks files out with CRLF
line ends turns them back into LF when it stores them, so each file is hashed
as Git would store it, and the sums come out the same on any checkout.
"""

import hashlib
import os

SUMS = "SHA256SUMS"
SENTINEL = "rp6502.cmake"


def as_git_stores(data):
    """data with each CRLF turned into LF, as Git's core.autocrlf stores a
    text file, unless Git's convert.c would take it for binary: a lone CR, a
    NUL, or more than one control character in 128 printable ones."""
    lonecr = nul = printable = nonprintable = 0
    i = 0
    while i < len(data):
        c = data[i]
        if c == 13:
            if i + 1 < len(data) and data[i + 1] == 10:
                i += 1
            else:
                lonecr += 1
        elif c == 10:
            pass
        elif c == 127:
            nonprintable += 1
        elif c < 32:
            if c in (8, 9, 27, 12):
                printable += 1
            else:
                nul += c == 0
                nonprintable += 1
        else:
            printable += 1
        i += 1
    if data.endswith(b"\x1a"):
        nonprintable -= 1
    if lonecr or nul or (printable >> 7) < nonprintable:
        return data
    return data.replace(b"\r\n", b"\n")


def main():
    tools = os.path.dirname(os.path.abspath(__file__))
    names = [
        name
        for name in os.listdir(tools)
        if name not in (SUMS, os.path.basename(__file__))
        and os.path.isfile(os.path.join(tools, name))
    ]
    names.sort(key=lambda name: (name == SENTINEL, name))
    lines = []
    for name in names:
        with open(os.path.join(tools, name), "rb") as file:
            data = as_git_stores(file.read())
        lines.append(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
    with open(os.path.join(tools, SUMS), "w", newline="\n") as file:
        file.writelines(lines)
    print(f"{SUMS}: {len(lines)} tools, {names[-1]} last")


if __name__ == "__main__":
    main()
