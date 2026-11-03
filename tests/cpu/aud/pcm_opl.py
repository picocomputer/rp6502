#!/usr/bin/env python3
# Copyright (c) 2026 WojciechGw
#
# for Rumbledethumps' Picocomputer 6502
#
# SPDX-License-Identifier: BSD-3-Clause

"""PCM drums and OPL2 music at once. A one-bar drum loop is streamed from
6502 RAM into the PCM ring, and the OPL2 sequencer steps every eighth note
of PCM actually played, counted from read_ptr, so the two never drift.
Any key exits."""

import math

from rp6502_asm import (API_OP, RIA_READY, RW0_ADDR, RW0_DATA, RW0_STEP,
                        RW1_ADDR, RW1_DATA, RW1_STEP, Asm, putc)
from rp6502_rom import main

RATE = 16000
EIGHTH = RATE // 4  # 120 BPM
BAR = EIGHTH * 8
PCM_PEAK = 56

PCM_SRC = 0x4000
PCM_PAGES = BAR // 256
assert BAR % 256 == 0 and PCM_SRC + BAR <= 0xFF00

XPCM = 0x0000
XRING = XPCM + 8
RING_LOG2 = 13
XOPL = 0xF000

TABLES = 0x2000
STEPS = 32

ZP_WPH = 0x10
ZP_SRC = 0x11
ZP_RPL = 0x13
ZP_RPH = 0x14
ZP_LRPL = 0x15
ZP_LRPH = 0x16
ZP_POSL = 0x17
ZP_POSH = 0x18
ZP_STEP = 0x19
ZP_PTR = 0x1A
ZP_DL = 0x1C
ZP_DH = 0x1D


class Lcg:
    def __init__(self, seed=1):
        self.x = seed

    def __call__(self):
        self.x = (self.x * 1103515245 + 12345) & 0x7FFFFFFF
        return self.x / 0x40000000 - 1.0


def drums():
    buf = [0.0] * BAR
    noise = Lcg(0x6502)

    def kick(at):
        phase = 0.0
        for i in range(min(3000, BAR - at)):
            t = i / RATE
            phase += 2 * math.pi * (50 + 100 * math.exp(-t / 0.03)) / RATE
            buf[at + i] += math.sin(phase) * math.exp(-t / 0.12)

    def snare(at):
        for i in range(min(2400, BAR - at)):
            t = i / RATE
            buf[at + i] += (noise() * 0.7 * math.exp(-t / 0.06) +
                            math.sin(2 * math.pi * 180 * t) * 0.5 *
                            math.exp(-t / 0.04))

    def hat(at, level):
        prev = 0.0
        for i in range(600):
            n = noise()
            buf[at + i] += (n - prev) * level * math.exp(-i / RATE / 0.015)
            prev = n

    for e, hit in enumerate("K.S.KKS."):
        at = e * EIGHTH
        if hit == "K":
            kick(at)
        if hit == "S":
            snare(at)
        hat(at, 0.3 if e & 1 else 0.18)

    peak = max(abs(v) for v in buf)
    return bytes(max(-128, min(127, round(v / peak * PCM_PEAK))) & 0xFF
                 for v in buf)


def fnum(note):
    f = 440.0 * 2 ** ((note - 69) / 12)
    for block in range(8):
        n = round(f * 2 ** (20 - block) / 49716)
        if n < 1024:
            return block, n
    raise ValueError(note)


MOD = (0x00, 0x01, 0x02, 0x08, 0x09)

# (0x20, 0x40, 0x60, 0x80) for modulator then carrier, and 0xC0.
BASS = ((0x01, 0x18, 0xF4, 0x55), (0x01, 0x0C, 0xF3, 0x47), 0x0A)
PAD = ((0x22, 0x26, 0x62, 0x14), (0x21, 0x1C, 0x52, 0x14), 0x01)
ARP = ((0x02, 0x20, 0xF6, 0x88), (0x01, 0x18, 0xF5, 0x86), 0x06)
VOICES = (BASS, PAD, PAD, PAD, ARP)

ROOTS = (45, 41, 48, 43)  # A2 F2 C3 G2
CHORDS = ((57, 60, 64), (53, 57, 60), (55, 60, 64), (55, 59, 62))
BASS_LINE = (0, 0, 12, 0, 0, 12, 7, 12)
ARP_LINE = (0, 1, 2, 1, 0, 1, 2, 1)


def opl_init():
    out = []
    for ch, (mod, car, fb) in enumerate(VOICES):
        for op, regs in ((MOD[ch], mod), (MOD[ch] + 3, car)):
            for base, val in zip((0x20, 0x40, 0x60, 0x80), regs):
                out += (base + op, val)
        out += (0xC0 + ch, fb)
    return out


def opl_note(ch, note):
    block, n = fnum(note)
    hi = (block << 2) | (n >> 8)
    return [0xB0 + ch, hi, 0xA0 + ch, n & 0xFF, 0xB0 + ch, 0x20 | hi]


def opl_step(s):
    bar, e = divmod(s, 8)
    out = opl_note(0, ROOTS[bar] + BASS_LINE[e])
    if e == 0:
        for v, note in enumerate(CHORDS[bar]):
            out += opl_note(1 + v, note)
    out += opl_note(4, CHORDS[bar][ARP_LINE[e]] + 12)
    return out


def tables():
    """The init list, the step lists, then the steps' lo and hi bytes."""
    lists = [opl_init()] + [opl_step(s) for s in range(STEPS)]
    blob, at = bytearray(), []
    for lst in lists:
        assert len(lst) < 256
        at.append(TABLES + len(blob))
        blob += bytes(lst) + b"\xff"
    lo = TABLES + len(blob)
    hi = lo + STEPS
    blob += bytes(a & 0xFF for a in at[1:])
    blob += bytes(a >> 8 for a in at[1:])
    assert TABLES + len(blob) <= PCM_SRC
    return bytes(blob), at[0], lo, hi


def program(init, step_lo, step_hi):
    p = Asm()
    loop, exit_ = p.local("loop"), p.local("exit")

    p.store(RW0_STEP, 1)
    p.say("\r\nPCM drums + OPL2 music. Any key exits.\r\n")

    p.xreg(0, 1, 1, XOPL)
    p.store(ZP_PTR, init & 0xFF)
    p.store(ZP_PTR + 1, init >> 8)
    p.jsr_abs("opl_list")

    for i, v in enumerate((0, 0, 0x03, RING_LOG2, RATE & 0xFF, RATE >> 8, 0, 0)):
        p.poke(XPCM + i, v)
    p.lda_imm(0)
    for zp in (ZP_WPH, ZP_SRC, ZP_LRPL, ZP_LRPH, ZP_POSL, ZP_POSH, ZP_STEP):
        p.sta_zp(zp)
    p.store(ZP_SRC + 1, PCM_SRC >> 8)
    p.xreg(0, 1, 2, XPCM)
    p.jsr_abs("opl_step")

    p.symbol(loop)
    p.bit_abs(RIA_READY)
    with p.branch("bvc"):
        p.jmp_abs(exit_)
    p.jsr_abs("read_rp")

    # write_ptr is always on a page, so the fill's low byte is only a borrow.
    p.sec()
    p.lda_imm(0)
    p.sbc_zp(ZP_RPL)
    p.lda_zp(ZP_WPH)
    p.sbc_zp(ZP_RPH)
    p.and_imm(0x1F)
    p.cmp_imm(0x1F)
    with p.branch("bcs"):
        p.jsr_abs("put_chunk")

    p.sec()
    p.lda_zp(ZP_RPL)
    p.sbc_zp(ZP_LRPL)
    p.sta_zp(ZP_DL)
    p.lda_zp(ZP_RPH)
    p.sbc_zp(ZP_LRPH)
    p.and_imm(0x1F)
    p.sta_zp(ZP_DH)
    p.lda_zp(ZP_RPL)
    p.sta_zp(ZP_LRPL)
    p.lda_zp(ZP_RPH)
    p.sta_zp(ZP_LRPH)
    p.clc()
    p.lda_zp(ZP_POSL)
    p.adc_zp(ZP_DL)
    p.sta_zp(ZP_POSL)
    p.lda_zp(ZP_POSH)
    p.adc_zp(ZP_DH)
    p.sta_zp(ZP_POSH)

    p.lda_zp(ZP_POSL)
    p.cmp_imm(EIGHTH & 0xFF)
    p.lda_zp(ZP_POSH)
    p.sbc_imm(EIGHTH >> 8)
    with p.branch("bcs"):
        p.jmp_abs(loop)
    p.sec()
    p.lda_zp(ZP_POSL)
    p.sbc_imm(EIGHTH & 0xFF)
    p.sta_zp(ZP_POSL)
    p.lda_zp(ZP_POSH)
    p.sbc_imm(EIGHTH >> 8)
    p.sta_zp(ZP_POSH)
    p.jsr_abs("opl_step")
    p.jmp_abs(loop)

    p.symbol(exit_)
    p.store(API_OP, 0xFF)
    p.jmp_abs(p.here())

    # The RIA writes read_ptr a byte at a time, so the high byte is read on
    # both sides of the low one.
    p.symbol("read_rp")
    p.lda_imm(0)
    p.sta_abs(RW1_STEP)
    p.sta_abs(RW1_ADDR + 1)
    p.symbol("read_rp.again")
    p.store(RW1_ADDR, XPCM + 7)
    p.lda_abs(RW1_DATA)
    p.sta_zp(ZP_RPH)
    p.store(RW1_ADDR, XPCM + 6)
    p.lda_abs(RW1_DATA)
    p.sta_zp(ZP_RPL)
    p.store(RW1_ADDR, XPCM + 7)
    p.lda_abs(RW1_DATA)
    p.cmp_zp(ZP_RPH)
    p.bne("read_rp.again")
    p.rts()

    p.symbol("put_chunk")
    p.store(RW0_ADDR, XRING & 0xFF)
    p.lda_zp(ZP_WPH)
    p.clc()
    p.adc_imm(XRING >> 8)
    p.sta_abs(RW0_ADDR + 1)
    p.ldy_imm(0)
    p.symbol("put_chunk.copy")
    p.lda_izy(ZP_SRC)
    p.sta_abs(RW0_DATA)
    p.iny()
    p.bne("put_chunk.copy")
    p.inc_zp(ZP_SRC + 1)
    p.lda_zp(ZP_SRC + 1)
    p.cmp_imm((PCM_SRC >> 8) + PCM_PAGES)
    with p.branch("bne"):
        p.store(ZP_SRC + 1, PCM_SRC >> 8)
    p.lda_zp(ZP_WPH)
    p.clc()
    p.adc_imm(1)
    p.and_imm((1 << (RING_LOG2 - 8)) - 1)
    p.sta_zp(ZP_WPH)
    p.store(RW0_ADDR, (XPCM + 1) & 0xFF)
    p.store(RW0_ADDR + 1, (XPCM + 1) >> 8)
    p.lda_zp(ZP_WPH)
    p.sta_abs(RW0_DATA)
    p.rts()

    p.symbol("opl_step")
    p.ldx_zp(ZP_STEP)
    p.lda_abx(step_lo)
    p.sta_zp(ZP_PTR)
    p.lda_abx(step_hi)
    p.sta_zp(ZP_PTR + 1)
    p.jsr_abs("opl_list")
    p.lda_zp(ZP_STEP)
    p.clc()
    p.adc_imm(1)
    p.and_imm(STEPS - 1)
    p.sta_zp(ZP_STEP)
    p.rts()

    p.symbol("opl_list")
    p.ldy_imm(0)
    p.symbol("opl_list.next")
    p.lda_izy(ZP_PTR)
    p.cmp_imm(0xFF)
    with p.branch("bne"):
        p.rts()
    p.sta_abs(RW0_ADDR)
    p.store(RW0_ADDR + 1, XOPL >> 8)
    p.iny()
    p.lda_izy(ZP_PTR)
    p.sta_abs(RW0_DATA)
    p.iny()
    p.jmp_abs("opl_list.next")

    p.use(putc)
    return p


def prog():
    blob, init, lo, hi = tables()
    p = program(init, lo, hi)
    assert p.here() <= TABLES, f"code ends at ${p.here():04X}"
    p.rom.add_binary_data(blob, TABLES)
    p.rom.add_binary_data(drums(), PCM_SRC)
    return p


ROMS = {"pcm+opl": prog}
main(globals())
