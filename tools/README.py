#!/usr/bin/env python3
#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
# SPDX-License-Identifier: Unlicense

"""The RP6502 project tools, as projects download them.

A new project starts with nothing but a small tools/rp6502.cmake that fetches
SHA256SUMS from this directory and downloads everything listed in it. That
replaces the fetcher with the real rp6502.cmake, which is why rp6502.cmake is
last: a project that got that far got everything before it. This script keeps
that order. Run it after changing any tool.

    python3 tools/README.py

Neither this file nor SHA256SUMS is a tool, so neither is listed and neither
ships. The emulator is not listed either; rp6502.cmake fetches it from a
release, into the project's tools directory rather than out of this one.
"""

import hashlib
import os

SUMS = "SHA256SUMS"
SENTINEL = "rp6502.cmake"


def main():
    tools = os.path.dirname(os.path.abspath(__file__))
    names = [
        name
        for name in os.listdir(tools)
        if name not in (SUMS, os.path.basename(__file__))
        and os.path.isfile(os.path.join(tools, name))
    ]
    # False sorts before True, so the sentinel lands last without being special
    # cased away when it is missing, which would write a list nobody can finish.
    names.sort(key=lambda name: (name == SENTINEL, name))
    lines = []
    for name in names:
        with open(os.path.join(tools, name), "rb") as file:
            lines.append(f"{hashlib.sha256(file.read()).hexdigest()}  {name}\n")
    with open(os.path.join(tools, SUMS), "w") as file:
        file.writelines(lines)
    print(f"{SUMS}: {len(lines)} tools, {names[-1]} last")


if __name__ == "__main__":
    main()
