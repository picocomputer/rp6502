#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import struct
import wave

import rp6502
from rp6502_asm import (API_OP, OP_ATTR_SET, RIA_TX, RW0_ADDR, RW0_DATA,
                        RW0_STEP, Asm)
from rp6502_rom import main, rom

PSG = 0x8000
GO = 0x0200


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
    page = PSG
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
    page = PSG
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
    """The note comes after a preamble of about 2,300 stores onto the OPL's
    page: the whole register file cleared, then register 1 written over and
    over. Every one of them reaches the chip where the program makes it, so
    the eleven writes of the note that follows sound. A machine that buffered
    them instead would have to hold the lot, and the eleven would be the ones
    it lost."""
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
    # A step of zero holds the address on one register, so the burst stays on
    # the page however long it runs.
    p.store(RW0_STEP, 0x00)
    p.store(RW0_ADDR, 0x01)
    p.store(RW0_ADDR + 1, page >> 8)
    out, inner = p.local("burst"), p.local("burst")
    p.lda_imm(0)
    p.ldy_imm(8)
    p.symbol(out)
    p.ldx_imm(0xFF)
    p.symbol(inner)
    p.sta_abs(RW0_DATA)
    p.dex()
    p.bne(inner)
    p.dey()
    p.bne(out)
    p.store(RW0_STEP, 0x01)
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


def bel_off_prog():
    """The bell is turned off and a BEL written. Once the script pokes GO,
    the bell is turned back on and a second BEL written."""
    p = Prog()
    p.store(GO, 0)
    p.pushl(0)
    p.call_a(OP_ATTR_SET, 5)  # ATTR_BEL
    p.lda_imm(0x07)
    p.sta_abs(RIA_TX)
    wait = p.local("wait")
    p.symbol(wait)
    p.lda_abs(GO)
    p.beq(wait)
    p.pushl(1)
    p.call_a(OP_ATTR_SET, 5)
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



# The recordings are 48 kHz, 800 samples to a frame. These bars are the ones
# the C audio tests used: one PSG voice at full volume peaks near 16127, far
# over LOUD, and BELL is a level no silence reaches.
FRAME = 800
LOUD = 4096
BELL = 32


def mono(path):
    """The wav's samples, left and right averaged."""
    with wave.open(path, "rb") as w:
        shape = (w.getnchannels(), w.getsampwidth(), w.getframerate())
        pcm = w.readframes(w.getnframes())
    if shape != (2, 2, 48000):
        raise ValueError(f"{path} is {shape}")
    samples = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    return [(samples[i] + samples[i + 1]) / 2 for i in range(0, len(samples), 2)]


def peaks(m):
    """The loudest sample of each frame."""
    return [max(abs(v) for v in m[i:i + FRAME]) for i in range(0, len(m), FRAME)]


def onset(p, start=0):
    """The first frame from start that is not silent, or -1."""
    return next((i for i in range(start, len(p)) if p[i]), -1)


def crossings(m):
    """Swings through a gate of 512 each way, the way the C tests counted."""
    cross, side = 0, 0
    for v in m:
        if v > 512:
            cross += side < 0
            side = 1
        elif v < -512:
            cross += side > 0
            side = -1
    return cross


def record(name, *lines):
    with rp6502.Emu(rom(name)) as e:
        for line in lines:
            e.cmd(line)


def test_psg():
    """The note starts within three frames, loud, and the twelve frames after
    its first are 440 Hz within a tenth. Then the gate byte is cleared twice.
    A plain poke changes XRAM, and the PSG, which catches the gate only as
    the RIA writes it, plays on. xbus: writes it the way the RIA does, and
    the note is released."""
    record("psg", 'wav "psg.wav" 15',
           "poke xram:PSG+6 $00", 'wav "plain.wav" 2',
           "poke xbus:PSG+6 $00", "run 1", 'wav "released.wav" 2')
    m = mono("psg.wav")
    p = peaks(m)
    at = onset(p)
    assert 0 <= at < 3 and p[at] > LOUD, \
        f"no loud note in frames 0-2: peaks {p[:3]}"
    tail = m[(at + 1) * FRAME:(at + 13) * FRAME]
    cross = crossings(tail)
    hz = cross / 2 * 48000 / len(tail)
    assert max(abs(v) for v in tail) > LOUD and cross > 100 \
        and 396 < hz < 484, f"{cross} crossings, {hz:.1f} Hz"
    assert max(peaks(mono("plain.wav"))) > LOUD, \
        "a plain poke of the gate released the note"
    assert not max(peaks(mono("released.wav"))), \
        "the note plays on after an xbus: poke of the gate"


def test_opl():
    """The chord starts within three frames, loud, and swings through the
    gate more than 80 times in the twelve frames after its first."""
    record("opl", 'wav "opl.wav" 15')
    m = mono("opl.wav")
    p = peaks(m)
    at = onset(p)
    assert 0 <= at < 3 and p[at] > LOUD, \
        f"no loud chord in frames 0-2: peaks {p[:3]}"
    tail = m[(at + 1) * FRAME:(at + 13) * FRAME]
    assert max(abs(v) for v in tail) > LOUD and crossings(tail) > 80, \
        f"{crossings(tail)} crossings"


def test_opl_init():
    """A burst that clears every OPL register before the note loses no write
    of the note's own."""
    record("opl_init", 'wav "init.wav" 3')
    p = peaks(mono("init.wav"))
    at = onset(p)
    assert at >= 0 and p[at] > LOUD, f"no loud note in frames 0-2: peaks {p}"


def test_psg_pre():
    """The block is programmed, gate and all, before xreg names it, so no note
    starts until the program writes the gate again about five frames later."""
    record("psg_pre", 'wav "pre.wav" 20')
    p = peaks(mono("pre.wav"))
    assert not any(p[1:4]), f"sound in frames 1-3: peaks {p[1:4]}"
    at = onset(p, 4)
    assert at >= 0 and p[at] > LOUD, \
        f"no loud note in frames 4-19: peaks {p[4:]}"


def test_bel():
    """A BEL rings with no sound generator mapped."""
    record("bel", 'wav "bel.wav" 4')
    p = peaks(mono("bel.wav"))
    at = onset(p)
    assert at >= 0 and p[at] > BELL, f"no bell in frames 0-3: peaks {p}"


def test_bel_off():
    """With the bell off, a BEL rings nothing. With it back on, a BEL rings."""
    record("bel_off", 'wait "\\a" 10', 'wav "off.wav" 16',
           "poke GO 1", 'wait "\\a" 10', 'wav "on.wav" 4')
    assert not max(peaks(mono("off.wav"))), "a BEL rang with the bell off"
    p = peaks(mono("on.wav"))
    assert max(p) > BELL, f"no bell once it was back on: peaks {p}"


def test_opl_bel():
    """A BEL rings over the OPL. The OPL alone is far louder than the bell,
    so the recording is compared with one of the same OPL code without the
    BEL, and what is left is the bell."""
    record("opl_bel", 'wav "both.wav" 4')
    record("opl", 'wav "opl.wav" 4')
    p = peaks([x - y for x, y in zip(mono("both.wav"), mono("opl.wav"))])
    at = onset(p)
    assert at >= 0 and p[at] > BELL, \
        f"no bell over the OPL in frames 0-3: peaks {p}"


def test_opl_exit():
    """The chord sounds, and the second frame after the program exits is
    silent."""
    record("opl_exit", 'wav "on.wav" 1', "wait exit 20", "expect exit 0",
           "run 1", 'wav "off.wav" 1')
    assert max(peaks(mono("on.wav"))), "no sound before the exit"
    assert not max(peaks(mono("off.wav"))), "sound after the exit"


ROMS = {"psg": psg_prog, "psg_pre": psg_pre_prog, "opl": opl_prog,
        "opl_exit": opl_exit_prog, "opl_init": opl_init_prog,
        "bel": bel_prog, "bel_off": bel_off_prog, "opl_bel": opl_bel_prog}
TESTS = {"psg": test_psg, "psg_pre": test_psg_pre, "opl": test_opl,
         "opl_exit": test_opl_exit, "opl_init": test_opl_init,
         "bel": test_bel, "bel_off": test_bel_off, "opl_bel": test_opl_bel}
main(globals())
