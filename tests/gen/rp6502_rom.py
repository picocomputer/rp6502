#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import importlib.util
import sys
from pathlib import Path

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


ROM = tool().ROM


class Rom(ROM):
    def record(self, addr, data):
        self.add_binary_data(bytes(data), addr)
        return self

    def reset(self, org=ORG):
        self.add_reset_vector(org)
        return self

    def program(self, prog, org=ORG):
        body = prog.code() if isinstance(prog, Asm) else bytes(prog)
        return self.record(org, body).reset(org)


def image(prog, org=ORG):
    return Rom().program(prog, org)
