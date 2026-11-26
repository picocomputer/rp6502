#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""Each way a name or an expression can fail ends the run with its own
message. A failure ends the run, so each case gets its own emulator. The
lookups in Python's rp6502.Symbols give the same values and messages as
the emulator's."""

import os
import subprocess

import rp6502
from rp6502_rom import main

# Lookups in adventure.sym, as (form, text, message): "" is a plain name.
# The emulator fails each with the message, as Python's KeyError does.
LOOKUPS = [
    ("", "LEVELS",
     "ambiguous name 'LEVELS': src/shop.c/LEVELS src/upgrades.c/LEVELS"),
    ("", "events",
     "ambiguous name 'events': src/game.c/events src/main.c/events"),
    ("address", "events", "address(events): events is in more than one "
     "file, so its address is not known; rename one"),
    ("address", "src/game.c/events", "address(src/game.c/events): events is "
     "in more than one file, so its address is not known; rename one"),
    ("sizeof", "hit", "ambiguous name 'hit': src/game.c/hit src/sound.c/hit"),
    ("countof", "hit", "ambiguous name 'hit': src/game.c/hit src/sound.c/hit"),
    ("sizeof", "tick", "ambiguous name 'tick': src/main.c/tick src/sound.c/tick"),
    ("countof", "tick", "countof(tick): tick is not an array"),
    ("sizeof", "src/main.c/tick", "sizeof(src/main.c/tick): tick has no size"),
    ("countof", "src/game.c/hit", "countof(src/game.c/hit): hit is not an array"),
    ("address", "XRAM_PSG", "address(XRAM_PSG): XRAM_PSG is a #define"),
    ("define", "table", "define(table): table is not a #define"),
    ("sizeof", "main", "sizeof(main): main has no size"),
    ("countof", "stdout", "countof(stdout): stdout is not an array"),
    ("", "gone", "gone is not in the linked program"),
    ("address", "gone", "address(gone): gone is not in the linked program"),
    ("sizeof", "src/shop.c/nope", "unknown name 'src/shop.c/nope'"),
    ("define", "src/other.c/LEVELS", "unknown name 'src/other.c/LEVELS'"),
    ("", "both", "ambiguous name 'both'"),
    ("", "L0010", "ambiguous name 'L0010'"),
    ("", "XRAM_PSGG", "unknown name 'XRAM_PSGG'"),
]

# Lookups that work, as (form, text, value).
FOUND = [
    ("sizeof", "table", 14), ("countof", "table", 7),
    ("sizeof", "src/sound.c/hit", 8), ("countof", "src/sound.c/hit", 8),
    ("sizeof", "src/game.c/hit", 1), ("define", "src/shop.c/LEVELS", 3),
    ("define", "src/upgrades.c/LEVELS", 5), ("sizeof", "events", 1),
    ("address", "table", 0x1500), ("define", "both", 0x10),
    ("address", "both", 0x1600), ("sizeof", "gone", 6), ("countof", "gone", 3),
    ("sizeof", "src/main.c/table", 14), ("define", "src/any.c/XRAM_PSG", 0x1200),
    ("address", "src/x.c/stdout", 0x1400), ("", "stdout", 0x1400),
    ("", "NEG", -20), ("", "src/shop.c/LEVELS", 3),
]


# Lookups in old.sym, which older tools wrote with a name and a value on each
# line. A name by itself works as it did, and a lookup is refused.
OLD = "the .sym was written by older tools; rebuild the project with the new ones"
OLD_LOOKUPS = [
    ("", "L0010", "ambiguous name 'L0010'"),
    ("", "NOPE", "unknown name 'NOPE'"),
    ("address", "SPOT", f"address(SPOT): {OLD}"),
    ("define", "SPOT", f"define(SPOT): {OLD}"),
    ("sizeof", "main", f"sizeof(main): {OLD}"),
    ("countof", "NOPE", f"countof(NOPE): {OLD}"),
    ("define", "src/game.c/SPOT", f"define(src/game.c/SPOT): {OLD}"),
]
OLD_FOUND = [("", "SPOT", 0x3000), ("", "NEG", -20), ("", "main", 0x0210)]


def shown(form, text):
    """The lookup as a script writes it."""
    return f"{form}({text})" if form else f"({text})"


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
    # A path is only for the text of a lookup. Outside one, / divides.
    (["poke xram:$1000 src/game.c/events"],
     "src/game.c/events: floats are not allowed, use //"),
    (["poke xram:$1000 sizeof(table"], "missing ) in 'sizeof(table'"),
    (["poke xram:$1000 (1 + sizeof( ))"], "bad expression '1 + sizeof( )'"),
] + [([f"poke xram:$1000 {shown(form, text)}"], message)
     for form, text, message in LOOKUPS] + [
    (["sys stop", 'load ":old.rp6502"', f"poke xram:$1000 {shown(form, text)}"],
     message) for form, text, message in OLD_LOOKUPS]


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


def check(sym, found, lookups):
    """The errors of the lookups in sym that do not give what they should."""
    errors = []
    for form, text, want in found:
        got = getattr(sym, form)(text) if form else sym[text]
        if got != want:
            errors.append(f"{shown(form, text)} is {got}, not {want}")
    for form, text, want in lookups:
        try:
            got = getattr(sym, form)(text) if form else sym[text]
            errors.append(f"{shown(form, text)} is {got}, not a KeyError")
        except KeyError as e:
            if e.args[0] != want:
                errors.append(f"{shown(form, text)} raises {e.args[0]!r}, not {want!r}")
    return errors


def test_symbols_python():
    with rp6502.Emu() as emu:
        sym = emu.sym
    errors = check(sym, FOUND, LOOKUPS)
    # A plain name with one value is in the dict, and only such a name.
    if "table" not in sym or "LEVELS" in sym or "gone" in sym:
        errors.append(f"the dict holds {sorted(sym)}")
    old = rp6502.Symbols(os.path.join(os.path.dirname(__file__), "symbols", "old.sym"))
    errors += check(old, OLD_FOUND, OLD_LOOKUPS + [
        ("", "src/game.c/SPOT", f"src/game.c/SPOT: {OLD}")])
    if "SPOT" not in old or "L0010" in old:
        errors.append(f"the dict of old.sym holds {sorted(old)}")
    assert not errors, "\n".join(errors)


TESTS = {"symbols_fail": test_symbols_fail,
         "symbols_python": test_symbols_python}
main(globals())
