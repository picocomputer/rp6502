#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""Installs on the null drive, checked by which program boots from it."""

import argparse
import os
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "gen"))
import rp6502_script  # noqa: E402


def replaces(emu, rom, here):
    """A second install of the same name, in another case, replaces the
    first, and a remove takes the name off."""
    (here / "adventure.rp6502").write_bytes(b"NOT THE ROM")
    fixture = Path(rom).resolve().as_posix()

    def body(e):
        e.cmd("sys stop")
        e.cmd('install "adventure.rp6502"')
        e.cmd(f'install "{fixture}" ADVENTURE.RP6502')
        e.cmd('load ":adventure.rp6502"')
        e.cmd('wait "Colossal Cave Adventure"')
        e.cmd("sys stop")
        e.cmd("remove adventure.rp6502")
        e.cmd(f'install "{fixture}"')
    return rp6502_script.drive(emu, rom, body, save_dir=here)


def host_path(emu, rom, here):
    """An install keeps its host path in UTF-8, so a ROM in a folder no code
    page can name still boots."""
    folder = here / "日本"
    folder.mkdir(exist_ok=True)
    shutil.copy(rom, folder / "z.rp6502")

    def body(e):
        e.cmd("sys stop")
        e.cmd(f'install "{(folder / "z.rp6502").as_posix()}"')
        e.cmd('load ":z.rp6502"')
        e.cmd('wait "Colossal Cave Adventure"')
        e.cmd("sys stop")
        e.cmd("remove :z.rp6502")
    return rp6502_script.drive(emu, rom, body, save_dir=here)


def drive(emu, rom, save_dir):
    here = Path(save_dir).resolve()
    os.chdir(here)
    code = replaces(emu, rom, here)
    # The host path case is POSIX only, as its C test was.
    if not code and os.name != "nt":
        code = host_path(emu, rom, here)
    return code


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--drive", action="store_true", help="run the cases")
    ap.add_argument("--emu", help="the rp6502-emu binary")
    ap.add_argument("--rom", help="adventure.rp6502")
    ap.add_argument("--save-dir", help="the folder behind SAVE:")
    a = ap.parse_args()
    if a.drive:
        return drive(a.emu, a.rom, a.save_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
