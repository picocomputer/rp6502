#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""Installs on the null drive, checked by which program boots from it."""

import os
import shutil
from pathlib import Path

import rp6502
from rp6502_rom import ADVENTURE, main, save_dir


def replaces(here):
    """A second install of the same name, in another case, replaces the
    first, and a remove takes the name off."""
    (here / "adventure.rp6502").write_bytes(b"NOT THE ROM")
    fixture = Path(ADVENTURE).as_posix()
    with rp6502.Emu(ADVENTURE) as e:
        e.cmd("sys stop")
        e.cmd('install "adventure.rp6502"')
        e.cmd(f'install "{fixture}" ADVENTURE.RP6502')
        e.cmd('load ":adventure.rp6502"')
        e.cmd('wait "Colossal Cave Adventure"')
        e.cmd("sys stop")
        e.cmd("remove adventure.rp6502")
        e.cmd(f'install "{fixture}"')


def host_path(here):
    """An install keeps its host path in UTF-8, so a ROM in a folder no code
    page can name still boots."""
    folder = here / "日本"
    folder.mkdir(exist_ok=True)
    shutil.copy(ADVENTURE, folder / "z.rp6502")
    with rp6502.Emu(ADVENTURE) as e:
        e.cmd("sys stop")
        e.cmd(f'install "{(folder / "z.rp6502").as_posix()}"')
        e.cmd('load ":z.rp6502"')
        e.cmd('wait "Colossal Cave Adventure"')
        e.cmd("sys stop")
        e.cmd("remove :z.rp6502")


def test_install():
    here = save_dir()
    os.chdir(here)
    replaces(here)
    # The host path case is POSIX only, as its C test was.
    if os.name != "nt":
        host_path(here)


TESTS = {"install": test_install}
main(globals())
