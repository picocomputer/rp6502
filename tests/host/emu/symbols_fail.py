#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""Each way a name or an expression can fail ends the run with its own
message. A failure ends the run, so each case gets its own emulator."""

import subprocess

import rp6502
from rp6502_rom import main

# The lines to send, and the message the last one fails with.
CASES = [
    (["poke xram:XRAM_PSGG $00"], "unknown name 'XRAM_PSGG'"),
    (["poke xram:L0010 $00"], "ambiguous name 'L0010'"),
    (["let XRAM_PSG = 1"], "cannot assign symbol 'XRAM_PSG'"),
    (["let base = XRAM_DATA_TAB", "poke xram:(base + 6 / 2) $01"],
     "base + 6 / 2: floats are not allowed, use //"),
    (["poke xram:$1000 (1.5 * 2)"], "1.5 * 2: floats are not allowed"),
    (["let base = 3e2"], "3e2: floats are not allowed"),
    (["let base = XRAM_DATA_TAB", "poke xram:base (base // 0)"],
     "base // 0 divides by zero"),
    (["poke xram:$1000 (1 << 63)"], "1 << 63 overflows 64 bits"),
    (["poke xram:$1000 (2 ** -1)"], "2 ** -1: floats are not allowed"),
    (["poke xram:$1000 (1 << -1)"], "1 << -1 shifts by a negative count"),
    (["let base = XRAM_DATA_TAB", "poke xram:base+ $00"],
     "bad expression 'base+'"),
    (["let base = XRAM_DATA_TAB", "poke xram:(base + 4 $00"],
     "missing ) in '(base + 4 $00'"),
    (["press NOPE"], "unknown key 'NOPE'"),
    (["let base"], "let wants name = expr"),
    (["let base = # NOPE"], "let wants name = expr"),
    # other.sym also has LATER, so after the load LATER is both a script
    # variable and a symbol.
    (["let LATER = 1", "sys stop", 'load ":other.rp6502"',
      "poke xram:LATER $00"], "ambiguous name 'LATER'"),
]


def test_symbols_fail():
    errors = []
    for lines, want in CASES:
        r = subprocess.run(rp6502.Emu.command("--script", "-"),
                           input="\n".join(lines) + "\n",
                           capture_output=True, text=True)
        # The emulator reports one failure, at the last line of the case.
        # A failure is two lines: the message, then the status.
        fails = [x for x in r.stderr.splitlines()
                 if x.startswith("rp6502-emu:")]
        line = f"rp6502-emu: <stdin>:{len(lines)}: {want}"
        if (r.returncode != 1 or len(fails) != 2 or fails[0] != line
                or not fails[1].startswith("rp6502-emu: PC=")):
            errors.append(f"{lines[-1]!r} should fail with {want!r}, but the "
                          f"emulator exited {r.returncode} with {r.stderr!r}")
    assert not errors, "\n".join(errors)


TESTS = {"symbols_fail": test_symbols_fail}
main(globals())
