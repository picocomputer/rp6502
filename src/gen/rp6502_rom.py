#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Writing a .rp6502, which six generators were each doing their own way.
#
# The container the loader reads: a magic line, then a record per block —
# an ASCII header naming the address, the length and a CRC, followed by
# the bytes. The program that goes in one is assembled by rp6502_asm.py,
# which is what a generator is actually for; the rest of it belongs to
# whatever question that generator asks.

import zlib
from pathlib import Path

from rp6502_asm import ORG, Asm


class Rom:
    """A .rp6502 under construction: records, in the order given."""

    def __init__(self):
        self.b = bytearray(b"#!RP6502\n")

    def record(self, addr, data):
        """The address is five digits so one format serves both the
        6502's sixteen bits and XRAM's seventeen; the loader scans hex
        and does not care about the leading zero."""
        data = bytes(data)
        crc = zlib.crc32(data) & 0xFFFFFFFF
        self.b += f"${addr:05X} ${len(data):X} ${crc:08X}\n".encode() + data
        return self

    def reset(self, org=ORG):
        """Where the 6502 starts, which every runnable image has to say."""
        return self.record(0xFFFC, bytes((org & 0xFF, org >> 8)))

    def program(self, prog, org=ORG):
        """The program and its reset vector, the two records every
        runnable image carries. Anything else — XRAM blocks, a second
        load address — is a record() before or after."""
        body = prog.code() if isinstance(prog, Asm) else bytes(prog)
        return self.record(org, body).reset(org)

    def write(self, path):
        Path(path).write_bytes(self.b)
        return len(self.b)


def image(prog, org=ORG):
    """The whole of the common case, in one call."""
    return Rom().program(prog, org)
