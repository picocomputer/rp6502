#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import importlib.util
import os
import sys
from pathlib import Path

import rp6502_asm
from rp6502_asm import ORG, Asm


def tool():
    path = Path(__file__).resolve().parents[2] / "tools" / "rp6502.py"
    spec = importlib.util.spec_from_file_location("rp6502_tool", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    cache, sys.dont_write_bytecode = sys.dont_write_bytecode, True
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.dont_write_bytecode = cache
    return mod


rp6502 = tool()
ROM = rp6502.ROM


class Rom(ROM):
    labels = {}

    def record(self, addr, data):
        self.add_binary_data(bytes(data), addr)
        return self

    def reset(self, org=ORG):
        self.add_reset_vector(org)
        return self

    def program(self, prog, org=ORG):
        if isinstance(prog, Asm):
            body = prog.code()
            self.labels = dict(prog._sym)
        else:
            body = bytes(prog)
        return self.record(org, body).reset(org)

    def write(self, path, names=None):
        """Writes the ROM and, beside it, its .sym file: the labels of the
        program and the whole numbers of rp6502_asm and of names."""
        size = super().write(path)
        pairs = list(self.labels.items())
        for table in (vars(rp6502_asm), names or {}):
            pairs += [(k, v) for k, v in table.items() if type(v) is int]
        rp6502.Symbols.write(os.path.splitext(str(path))[0] + ".sym", pairs)
        return size


def image(prog, org=ORG):
    return Rom().program(prog, org)
