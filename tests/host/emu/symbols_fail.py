#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""Each way a name or an expression can fail ends the run with its own
message. A failure ends the run, so each case gets its own emulator."""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "gen"))
import rp6502_script  # noqa: E402

# The lines to send, and the message the last one fails with.
CASES = [
    (["poke xram:XRAM_PSGG $00"], "unknown name 'XRAM_PSGG'"),
    (["poke xram:L0010 $00"], "ambiguous name 'L0010'"),
    (["let XRAM_PSG = 1"], "cannot assign symbol 'XRAM_PSG'"),
    (["let base = XRAM_DATA_TAB", "poke xram:(base + 6 / 2) $01"],
     "base + 6 / 2 is not a whole number"),
    (["let base = XRAM_DATA_TAB", "poke xram:base (base // 0)"],
     "base // 0 divides by zero"),
    (["poke xram:$1000 (1 << 63)"], "1 << 63 overflows 64 bits"),
    (["poke xram:$1000 ((-8.0) ** (1 / 3))"],
     "(-8.0) ** (1 / 3) is a complex number"),
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


def drive(emu, rom, save_dir=None):
    args = ("--install", str(Path(rom).with_name("other.rp6502")))
    if save_dir:
        args = ("--save-dir", str(save_dir), *args)
    failed = 0
    for lines, want in CASES:
        e = rp6502_script.Emu(emu, rom, args)
        with e:
            e.start()
            got = "no failure"
            try:
                for line in lines:
                    e.cmd(line)
            except rp6502_script.ScriptError as err:
                got = str(err)
        code = e.close()
        # One failure gives one fail line, never a second one after it.
        if (code != 1 or not got.startswith(f"{lines[-1]!r}: {want}; ")
                or "\nfail " in got):
            print(f"{sys.argv[0]}: {lines[-1]!r} should fail with {want!r}, "
                  f"but the emulator exited {code} with {got!r}",
                  file=sys.stderr)
            failed = 1
    return failed


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--drive", action="store_true", help="run every case")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="the ROM to run")
    ap.add_argument("--save-dir", help="the folder behind SAVE:")
    a = ap.parse_args()
    if a.drive:
        return drive(a.emu, a.rom, a.save_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
