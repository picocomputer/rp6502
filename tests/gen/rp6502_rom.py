#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Writing a .rp6502, which six generators were each doing their own way.
#
# The format is tools/rp6502.py's, because that is the tool a project uses
# to build one and the only writer the documentation describes. This was a
# second writer, and its images were ones the tool it shipped beside could
# not read back: a generated ROM could never be merged, or run through
# `rp6502.py run`. Worse, it wrote the whole program as one record, and
# the monitor refuses a record over MBUF_SIZE — so the two ROMs whose
# whole reason to exist is being run on hardware were the two that would
# not load there.
#
# What is left here is the shorthand: the two records every runnable image
# carries, named in one call.
#
# The program that goes in one is assembled by rp6502_asm.py, which is
# what a generator is actually for; the rest of it belongs to whatever
# question that generator asks.

import importlib.util
import sys
from pathlib import Path

from rp6502_asm import ORG, Asm


def tool():
    """tools/rp6502.py, loaded by path: it is a script with a generic name,
    and importing it as a module would put "rp6502" on the path for
    everything else. The same reason rp6502_asm.py reaches the vendor's
    decode table this way."""
    path = Path(__file__).resolve().parents[2] / "tools" / "rp6502.py"
    spec = importlib.util.spec_from_file_location("rp6502_tool", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    # tools/ is the directory a project downloads, and CI checks that what is
    # in it is exactly what SHA256SUMS lists. Importing writes a __pycache__
    # there, which is neither a tool nor listed.
    cache, sys.dont_write_bytecode = sys.dont_write_bytecode, True
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.dont_write_bytecode = cache
    return mod


ROM = tool().ROM


class Rom(ROM):
    """A .rp6502 under construction: the tool's builder, with the shapes a
    generator wants."""

    def record(self, addr, data):
        self.add_binary_data(bytes(data), addr)
        return self

    def reset(self, org=ORG):
        """Where the 6502 starts, which every runnable image has to say."""
        self.add_reset_vector(org)
        return self

    def program(self, prog, org=ORG):
        """The program and its reset vector, the two records every
        runnable image carries. Anything else — XRAM blocks, a second
        load address — is a record() before or after."""
        body = prog.code() if isinstance(prog, Asm) else bytes(prog)
        return self.record(org, body).reset(org)


def image(prog, org=ORG):
    """The whole of the common case, in one call."""
    return Rom().program(prog, org)
