#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Two .rp6502 programs that ask an audio device for one note and then do
# nothing at all. They exist because the machine's own diagnostics print
# to the console, and every program that drives the OPL2 switches to a
# canvas where the console cannot be read — so the one thing needed to
# debug audio on hardware was a program that makes a sound and leaves
# the screen alone.
#
# tests/aud/test_aud.cpp runs these same files, so what sounds on a
# Pocket and what the simulation asserts cannot drift apart.

import argparse
import zlib
from pathlib import Path

ORG = 0x0300

# RIA registers the programs touch.
RW0_DATA = 0xFFE4
RW0_ADDR = 0xFFE6
XSTACK = 0xFFEC
UART_TX = 0xFFE1
API_OP = 0xFFEF
API_CALL = 0xFFF1


class Prog:
    def __init__(self):
        self.b = bytearray()

    def lda(self, v):
        self.b += bytes((0xA9, v))

    def sta(self, a):
        self.b += bytes((0x8D, a & 0xFF, a >> 8))

    def push(self, v):
        self.lda(v)
        self.sta(XSTACK)

    def pushw(self, w):
        self.push((w >> 8) & 0xFF)
        self.push(w & 0xFF)

    def xreg(self, dev, ch, addr, word):
        self.push(dev)
        self.push(ch)
        self.push(addr)
        self.pushw(word)
        self.lda(0x01)
        self.sta(API_OP)
        self.b += bytes((0x20, API_CALL & 0xFF, API_CALL >> 8))

    def poke(self, addr, val):
        """One XRAM byte through RW0, which is what the device snoops."""
        self.lda(addr & 0xFF)
        self.sta(RW0_ADDR)
        self.lda(addr >> 8)
        self.sta(RW0_ADDR + 1)
        self.lda(val)
        self.sta(RW0_DATA)

    def settle(self):
        """About 1,280 cycles. The PSG applies its pointer at a sample
        boundary rather than on the write, so a gate edge landing in the
        period right after xreg returns is never snooped."""
        self.b += bytes((0xA2, 0x00, 0xCA, 0xD0, 0xFD))

    def spin(self):
        here = ORG + len(self.b)
        self.b += bytes((0x4C, here & 0xFF, here >> 8))

    def delay(self, outer):
        """About outer * 1,280 cycles of nothing."""
        self.b += bytes((0xA0, outer & 0xFF))       # LDY #outer
        self.b += bytes((0xA2, 0x00))               # outer: LDX #0
        self.b += bytes((0xCA, 0xD0, 0xFD))         # inner: DEX BNE inner
        self.b += bytes((0x88, 0xD0, 0xF8))         # DEY BNE outer (to the LDX)

    def exit(self):
        """API exit: the firmware parks both audio engines and stops the
        CPU, so anything still sounding past this is the machine's bug."""
        self.lda(0xFF)
        self.sta(API_OP)
        self.spin()


def psg_prog():
    p = Prog()
    page = 0x8000
    p.xreg(0, 1, 0, page)
    p.settle()
    # 440 Hz — the engine divides by three — half duty, and the loudest
    # volume, which is index zero: the nibble selects into a table that
    # runs 256 down to 0, so it attenuates and 15 is silence.
    p.poke(page + 0, 0x28)
    p.poke(page + 1, 0x05)
    p.poke(page + 2, 0x80)
    p.poke(page + 3, 0x00)
    p.poke(page + 4, 0x00)
    p.poke(page + 5, 0x00)
    p.poke(page + 6, 0x01)  # gate last: this edge is what starts the note
    p.spin()
    return p.b


def opl_prog():
    p = Prog()
    page = 0xF000
    p.xreg(0, 1, 1, page)
    # Channel 0 of a YM3812: modulator is slot 0, carrier slot 3.
    for reg, val in (
        (0x20, 0x01),  # modulator multiple 1
        (0x23, 0x01),  # carrier multiple 1
        (0x40, 0x10),  # modulator level
        (0x43, 0x00),  # carrier at full
        (0x60, 0xF0),  # fast attack
        (0x63, 0xF0),
        (0x80, 0x77),  # sustain and release
        (0x83, 0x77),
        (0xC0, 0x0E),  # feedback, both operators to the output
        (0xA0, 0x98),  # f-number low
        (0xB0, 0x31),  # key on, block 4, f-number high
    ):
        p.poke(page + reg, val)
    p.spin()
    return p.b


def opl_exit_prog():
    """The OPL note, a moment of it, then a clean program exit. test_aud
    holds the machine to silence afterwards — the coverage the stopped-
    program bug never had, on the platform where the engines free-run."""
    p = Prog()
    page = 0xF000
    p.xreg(0, 1, 1, page)
    for reg, val in (
        (0x20, 0x01), (0x23, 0x01), (0x40, 0x10), (0x43, 0x00),
        (0x60, 0xF0), (0x63, 0xF0), (0x80, 0x77), (0x83, 0x77),
        (0xC0, 0x0E), (0xA0, 0x98), (0xB0, 0x31),
    ):
        p.poke(page + reg, val)
    p.delay(64)
    p.exit()
    return p.b


def bel_prog():
    """One BEL character and nothing else. The bell is a voice of the PSG
    that the soft CPU drives, so this proves it sounds with no program
    holding an engine — which is the console's own case."""
    p = Prog()
    p.lda(0x07)
    p.sta(UART_TX)
    p.spin()
    return p.b


def opl_bel_prog():
    """The OPL note and a BEL over the top of it. Every engine is audible
    at all times, and this is the sharp end of that: two voices from two
    places reaching the codec on one tick."""
    p = Prog()
    page = 0xF000
    p.xreg(0, 1, 1, page)
    for reg, val in (
        (0x20, 0x01), (0x23, 0x01), (0x40, 0x10), (0x43, 0x00),
        (0x60, 0xF0), (0x63, 0xF0), (0x80, 0x77), (0x83, 0x77),
        (0xC0, 0x0E), (0xA0, 0x98), (0xB0, 0x31),
    ):
        p.poke(page + reg, val)
    p.lda(0x07)
    p.sta(UART_TX)
    p.spin()
    return p.b


def record(addr, data):
    head = f"${addr:05X} ${len(data):X} ${zlib.crc32(data) & 0xFFFFFFFF:08X}\n"
    return head.encode() + data


def emit(path, body):
    rom = b"#!RP6502\n"
    rom += record(ORG, body)
    rom += record(0xFFFC, bytes((ORG & 0xFF, ORG >> 8)))
    Path(path).write_bytes(rom)
    return len(rom)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit-psg")
    ap.add_argument("--emit-opl")
    ap.add_argument("--emit-opl-exit")
    ap.add_argument("--emit-bel")
    ap.add_argument("--emit-opl-bel")
    a = ap.parse_args()
    if a.emit_psg:
        print(f"psg.rp6502 {emit(a.emit_psg, psg_prog())} bytes")
    if a.emit_opl:
        print(f"opl.rp6502 {emit(a.emit_opl, opl_prog())} bytes")
    if a.emit_opl_exit:
        print(f"opl_exit.rp6502 {emit(a.emit_opl_exit, opl_exit_prog())} bytes")
    if a.emit_bel:
        print(f"bel.rp6502 {emit(a.emit_bel, bel_prog())} bytes")
    if a.emit_opl_bel:
        print(f"opl_bel.rp6502 {emit(a.emit_opl_bel, opl_bel_prog())} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
