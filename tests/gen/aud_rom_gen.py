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
# tests/rtl/aud/test_aud.cpp runs these same files, so what sounds on a
# Pocket and what the simulation asserts cannot drift apart.

import argparse

from rp6502_asm import API_OP, RIA_TX, RW0_ADDR, RW0_DATA, Asm
from rp6502_rom import image


class Prog(Asm):
    """The base assembler plus the two things only an audio program
    needs: waiting out a note, and a clean exit that proves the
    firmware parks the engines."""

    def settle(self):
        """About 1,280 cycles. The pointer's reset releases every gate at
        the next sample boundary, so a gate edge landing in the period
        right after xreg returns is taken back."""
        loop = self.local("settle")
        self.ldx_imm(0)
        self.symbol(loop)
        self.dex()
        self.bne(loop)

    def spin(self):
        self.jmp_abs(self.here())

    def delay(self, outer):
        """About outer * 1,280 cycles of nothing."""
        out, inner = self.local("delay"), self.local("delay")
        self.ldy_imm(outer & 0xFF)
        self.symbol(out)
        self.ldx_imm(0)
        self.symbol(inner)
        self.dex()
        self.bne(inner)
        self.dey()
        self.bne(out)

    def exit(self):
        """API exit: the firmware parks both audio engines and stops the
        CPU, so anything still sounding past this is the machine's bug."""
        self.lda_imm(0xFF)
        self.sta_abs(API_OP)
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
    return p


def psg_pre_prog():
    """The same note, programmed the other way round: the whole block
    written before the device is asked for, gate included. The engine
    hears only writes, so those bytes reach it by the import the
    firmware runs on the pointer — and the gate among them must not
    sound, because the machine that reads XRAM instead throws its own
    gate queue away at exactly that moment. The note starts on the one
    gate written afterwards."""
    p = Prog()
    page = 0x8000
    p.poke(page + 0, 0x28)
    p.poke(page + 1, 0x05)
    p.poke(page + 2, 0x80)
    p.poke(page + 3, 0x00)
    p.poke(page + 4, 0x00)
    p.poke(page + 5, 0x00)
    p.poke(page + 6, 0x01)
    p.xreg(0, 1, 0, page)
    p.settle()
    # About five frames of it, so the silence is something a test can
    # stand in the middle of rather than infer from a frame count.
    p.delay(255)
    p.delay(255)
    p.poke(page + 6, 0x01)
    p.spin()
    return p


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
    return p


def opl_exit_prog():
    """The OPL note, held longer than a frame, then a clean program exit.
    test_aud holds the machine to silence afterwards — the coverage the
    stopped-program bug never had, on the platform where the engines
    free-run. Longer than a frame because the emulator's sink pulls once a
    frame and a note that starts and ends between two pulls is never
    heard; the fabric hears it either way."""
    p = Prog()
    page = 0xF000
    p.xreg(0, 1, 1, page)
    for reg, val in (
        (0x20, 0x01), (0x23, 0x01), (0x40, 0x10), (0x43, 0x00),
        (0x60, 0xF0), (0x63, 0xF0), (0x80, 0x77), (0x83, 0x77),
        (0xC0, 0x0E), (0xA0, 0x98), (0xB0, 0x31),
    ):
        p.poke(page + reg, val)
    p.delay(128)
    p.exit()
    return p


def opl_init_prog():
    """The note after the clearing burst a real OPL2 program opens with:
    a store to every register, as fast as the 6502 can make them through
    a stepping RW0, then the note. The queue that carries a program's
    writes to the engine holds 255, so a machine that drains it once a
    frame loses the key-on at the end of this and the note with it --
    which is how every real OPL2 program went silent on the emulator
    while these eleven-write ones played."""
    p = Prog()
    page = 0xF000
    p.xreg(0, 1, 1, page)
    p.store(RW0_ADDR, 0x01)
    p.store(RW0_ADDR + 1, page >> 8)
    clear = p.local("clear")
    p.lda_imm(0)
    p.ldx_imm(0xFF)
    p.symbol(clear)
    p.sta_abs(RW0_DATA)
    p.dex()
    p.bne(clear)
    for reg, val in (
        (0x20, 0x01), (0x23, 0x01), (0x40, 0x10), (0x43, 0x00),
        (0x60, 0xF0), (0x63, 0xF0), (0x80, 0x77), (0x83, 0x77),
        (0xC0, 0x0E), (0xA0, 0x98), (0xB0, 0x31),
    ):
        p.poke(page + reg, val)
    p.spin()
    return p


def bel_prog():
    """One BEL character and nothing else. The bell is a voice of the PSG
    that the soft CPU drives, so this proves it sounds with no program
    holding an engine — which is the console's own case."""
    p = Prog()
    p.lda_imm(0x07)
    p.sta_abs(RIA_TX)
    p.spin()
    return p


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
    p.lda_imm(0x07)
    p.sta_abs(RIA_TX)
    p.spin()
    return p


def emit(path, body):
    return image(body).write(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit-psg")
    ap.add_argument("--emit-psg-pre")
    ap.add_argument("--emit-opl")
    ap.add_argument("--emit-opl-exit")
    ap.add_argument("--emit-opl-init")
    ap.add_argument("--emit-bel")
    ap.add_argument("--emit-opl-bel")
    a = ap.parse_args()
    if a.emit_psg:
        print(f"psg.rp6502 {emit(a.emit_psg, psg_prog())} bytes")
    if a.emit_psg_pre:
        print(f"psg_pre.rp6502 {emit(a.emit_psg_pre, psg_pre_prog())} bytes")
    if a.emit_opl:
        print(f"opl.rp6502 {emit(a.emit_opl, opl_prog())} bytes")
    if a.emit_opl_exit:
        print(f"opl_exit.rp6502 {emit(a.emit_opl_exit, opl_exit_prog())} bytes")
    if a.emit_opl_init:
        print(f"opl_init.rp6502 {emit(a.emit_opl_init, opl_init_prog())} bytes")
    if a.emit_bel:
        print(f"bel.rp6502 {emit(a.emit_bel, bel_prog())} bytes")
    if a.emit_opl_bel:
        print(f"opl_bel.rp6502 {emit(a.emit_opl_bel, opl_bel_prog())} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
