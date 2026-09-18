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
