#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse

from rp6502_asm import API_OP, RIA_TX, RW0_ADDR, RW0_DATA, Asm
from rp6502_rom import image


class Prog(Asm):
    def settle(self):
        """The loop takes about 1,280 cycles, which is 160 us at the
        default 8 MHz. psg.sv releases its eight program voices at most one
        48 kHz sample period of 20.8 us after its pointer register is
        written, and a gate written before then is released with them. The
        pointer is written before the xreg call returns, so a gate written
        after this loop starts its note."""
        loop = self.local("settle")
        self.ldx_imm(0)
        self.symbol(loop)
        self.dex()
        self.bne(loop)

    def spin(self):
        self.jmp_abs(self.here())

    def delay(self, outer):
        """The delay lasts about outer * 1,286 cycles."""
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
        self.lda_imm(0xFF)
        self.sta_abs(API_OP)
        self.spin()


def psg_prog():
    p = Prog()
    page = 0x8000
    p.xreg(0, 1, 0, page)
    p.settle()
    # The frequency register holds hertz times three, so 0x0528 is 440 Hz.
    # A duty of 0x80 is half of each cycle. Volume 0 is the loudest because
    # the volume nibble selects from a table that runs from 256 down to 0,
    # so volume 15 is silence.
    p.poke(page + 0, 0x28)
    p.poke(page + 1, 0x05)
    p.poke(page + 2, 0x80)
    p.poke(page + 3, 0x00)
    p.poke(page + 4, 0x00)
    p.poke(page + 5, 0x00)
    p.poke(page + 6, 0x01)  # gate on, which starts the note
    p.spin()
    return p


def psg_pre_prog():
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
    # The two delays last about 656,000 cycles, which is about five frames
    # at the default 8 MHz, so a test can measure whole frames of silence
    # before the note starts.
    p.delay(255)
    p.delay(255)
    p.poke(page + 6, 0x01)
    p.spin()
    return p


def opl_prog():
    p = Prog()
    page = 0xF000
    p.xreg(0, 1, 1, page)
    # In the YM3812's register layout, channel 0's modulator is at operator
    # offset 0 and its carrier is at offset 3.
    for reg, val in (
        (0x20, 0x01),  # modulator multiple 1
        (0x23, 0x01),  # carrier multiple 1
        (0x40, 0x10),  # modulator total level 16
        (0x43, 0x00),  # carrier at full
        (0x60, 0xF0),  # fast attack
        (0x63, 0xF0),
        (0x80, 0x77),  # sustain level 7, release rate 7
        (0x83, 0x77),
        (0xC0, 0x0E),  # feedback 7, modulator into the carrier
        (0xA0, 0x98),  # f-number low
        (0xB0, 0x31),  # key on, block 4, f-number high
    ):
        p.poke(page + reg, val)
    p.spin()
    return p


def opl_exit_prog():
    """The note is held for about 164,600 cycles, which is longer than a
    frame at the default 8 MHz, before the program exits. The emulator
    runs the engines only inside aud_render, and test_aud.c calls it once
    a frame, so a note that starts and is stopped between two calls is
    never mixed."""
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
    """The note comes after a burst of 255 stores through RW0. opl_sample
    in opl.c takes RW0 and RW1 writes to the OPL's XRAM page from a queue
    that holds 255 entries, so the eleven writes of the note are dropped
    unless the queue drains while the program writes."""
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
    p = Prog()
    p.lda_imm(0x07)
    p.sta_abs(RIA_TX)
    p.spin()
    return p


def opl_bel_prog():
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
