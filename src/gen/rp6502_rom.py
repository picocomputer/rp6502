#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Writing a .rp6502, which six generators were each doing their own way.
#
# Two things live here. Rom is the container the loader reads: a magic
# line, then a record per block — an ASCII header naming the address,
# the length and a CRC, followed by the bytes. Asm is enough 65C02 to
# write the program that goes in one, which is what a generator is
# actually for; the rest of it belongs to whatever question that
# generator asks.
#
# tests/bench/tb_rom.h and tests/bench/tb_asm.h are this file in C++,
# for the benches that build an image in memory rather than on disk.
# There is one container format and one instruction encoding, so a
# program reads the same in either language.
#
# Addressing is absolute throughout. A program lives at $0300 and talks
# to $FFxx, so zero page would save bytes nobody is counting and cost a
# second spelling of every instruction.

import zlib
from pathlib import Path

# Where a program is loaded and started.
ORG = 0x0300

# The RIA register window, as the 6502 sees it.
RIA_READY = 0xFFE0
RIA_TX = 0xFFE1
RIA_RX = 0xFFE2
RW0_DATA = 0xFFE4
RW0_ADDR = 0xFFE6
XSTACK = 0xFFEC
API_ERRNO = 0xFFED
API_OP = 0xFFEF
API_CALL = 0xFFF1
API_A = 0xFFF4
API_X = 0xFFF6

# The API ops a generated program reaches for. These are the machine's
# numbers, not any one program's, which is why they are not in the
# generator that happened to need them first.
OP_XREG = 0x01
OP_OPEN = 0x14
OP_CLOSE = 0x15
OP_READ_XSTACK = 0x16
OP_WRITE_XSTACK = 0x18
OP_LSEEK = 0x1A

O_RDONLY = 0x01
O_WRONLY = 0x02
O_RDWR = 0x03
O_CREAT = 0x10
O_TRUNC = 0x20


class Asm:
    """A 65C02 program under construction, assembled at ORG."""

    def __init__(self, org=ORG):
        self.org = org
        self.b = bytearray()

    # --- the encoding ---

    def emit(self, *by):
        self.b += bytes(b & 0xFF for b in by)

    def here(self):
        """The address the next instruction will be assembled at."""
        return self.org + len(self.b)

    def abs(self, op, a):
        self.emit(op, a & 0xFF, a >> 8)

    def lda(self, v):
        self.emit(0xA9, v)

    def ldx(self, v):
        self.emit(0xA2, v)

    def ldy(self, v):
        self.emit(0xA0, v)

    def lda_abs(self, a):
        self.abs(0xAD, a)

    def ldx_abs(self, a):
        self.abs(0xAE, a)

    def cmp_abs(self, a):
        self.abs(0xCD, a)

    def ora_abs(self, a):
        self.abs(0x0D, a)

    def inc_abs(self, a):
        self.abs(0xEE, a)

    def sta(self, a):
        self.abs(0x8D, a)

    def stx(self, a):
        self.abs(0x8E, a)

    def bit(self, a):
        self.abs(0x2C, a)

    def jsr(self, a):
        self.abs(0x20, a)

    def jmp(self, a):
        self.abs(0x4C, a)

    def inx(self):
        self.emit(0xE8)

    def dex(self):
        self.emit(0xCA)

    def rts(self):
        self.emit(0x60)

    def stp(self):
        self.emit(0xDB)

    def store(self, a, v):
        self.lda(v)
        self.sta(a)

    def branch(self, op):
        """Emit a forward branch and return a handle that closes it."""
        self.emit(op, 0x00)
        return len(self.b)

    def close(self, h):
        self.b[h - 1] = len(self.b) - h

    def inc16(self, lo, hi):
        """++[hi:lo], carrying without disturbing A."""
        self.inc_abs(lo)
        self.emit(0xD0, 0x03)  # bne over
        self.inc_abs(hi)

    # --- the machine ---

    def push(self, v):
        """The xstack grows down, so the last byte pushed is the first
        the API pops: a word goes high byte first, a string backwards."""
        self.store(XSTACK, v)

    def pushw(self, w):
        self.push((w >> 8) & 0xFF)
        self.push(w & 0xFF)

    def pushl(self, v):
        self.push((v >> 24) & 0xFF)
        self.push((v >> 16) & 0xFF)
        self.push((v >> 8) & 0xFF)
        self.push(v & 0xFF)

    def push_str(self, s):
        """latin-1, which is to say the byte you wrote. The machine reads
        a filename in its code page, so str.encode()'s UTF-8 default
        would push two bytes for anything over 0x7F and hand the API a
        name it never spelled. ASCII is the same either way, which is
        why this went unnoticed until the C++ spelling disagreed."""
        if isinstance(s, str):
            s = s.encode("latin-1")
        for c in reversed(s + b"\0"):
            self.push(c)

    def call(self, op):
        """An API call: the op, then the trampoline the 6502 spins in."""
        self.store(API_OP, op)
        self.jsr(API_CALL)

    def call_a(self, op, a):
        """With the attribute id, which every op that takes one puts in A."""
        self.store(API_A, a)
        self.call(op)

    def xreg(self, dev, ch, addr, *words):
        self.push(dev)
        self.push(ch)
        self.push(addr)
        for w in words:
            self.pushw(w)
        self.call(OP_XREG)

    def poke(self, addr, val):
        """One XRAM byte through RW0, which is the port a device snoops."""
        self.store(RW0_ADDR, addr & 0xFF)
        self.store(RW0_ADDR + 1, addr >> 8)
        self.store(RW0_DATA, val)

    def putc_a(self):
        """Print the accumulator, waiting for the TX ready bit."""
        self.emit(0x48)  # pha
        self.bit(RIA_READY)
        self.emit(0x10, 0xFB)  # bpl -5
        self.emit(0x68)  # pla
        self.sta(RIA_TX)


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
        body = prog.b if isinstance(prog, Asm) else prog
        return self.record(org, body).reset(org)

    def write(self, path):
        Path(path).write_bytes(self.b)
        return len(self.b)


def image(prog, org=ORG):
    """The whole of the common case, in one call."""
    return Rom().program(prog, org)
