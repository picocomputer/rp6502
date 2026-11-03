#!/usr/bin/env python3
# Copyright (c) 2026 WojciechGw
#
# for Rumbledethumps' Picocomputer 6502
#
# SPDX-License-Identifier: BSD-3-Clause

"""PCM drums with OPL2 or PSG music at once. A one-bar drum loop is
streamed from 6502 RAM into the PCM ring, and the sequencer steps every
eighth note of PCM actually played, counted from read_ptr, so the two never
drift. Space switches the music between the OPL2 and the PSG while the
drums carry on; Q exits."""

import math

from rp6502_asm import (API_OP, RIA_READY, RIA_RX, RW0_ADDR, RW0_DATA,
                        RW0_STEP, RW1_ADDR, RW1_DATA, RW1_STEP, Asm, putc)
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
XPSG = 0xF100

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
ZP_MODE = 0x1E  # 0 for the OPL2, STEPS for the PSG
ZP_PAGE = 0x1F
ZP_TMP = 0x20


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


# (wave_release, duty, vol_attack, vol_decay, pan) per channel. Volume 0 is
# the loudest, and a pan byte of 0x80 would drop the channel from the mix.
PSG_BASS = (0x33, 0xFF, 0x40, 0x65, 0x00)
PSG_ARP = (0x13, 0x40, 0x80, 0xC4, 0x00)
PSG_VOICES = (PSG_BASS,
              (0x29, 0xFF, 0x95, 0xA8, 0xD0),
              (0x29, 0xFF, 0x95, 0xA8, 0x00),
              (0x29, 0xFF, 0x95, 0xA8, 0x30),
              PSG_ARP)


def psg_init():
    out = []
    for ch, (wr, duty, va, vd, pan) in enumerate(PSG_VOICES):
        o = ch * 8
        out += (o + 2, duty, o + 3, va, o + 4, vd, o + 5, wr, o + 6, pan)
    return out


def psg_note(ch, note):
    """Gate off then on, which is what starts the attack."""
    f = round(440.0 * 2 ** ((note - 69) / 12) * 3)
    o, pan = ch * 8, PSG_VOICES[ch][4]
    return [o + 6, pan, o + 0, f & 0xFF, o + 1, f >> 8, o + 6, pan | 1]


def chord(note, bar):
    out = []
    for v, n in enumerate(CHORDS[bar]):
        out += note(1 + v, n)
    return out


def step(note, s):
    bar, e = divmod(s, 8)
    out = note(0, ROOTS[bar] + BASS_LINE[e])
    if e == 0:
        out += chord(note, bar)
    out += note(4, CHORDS[bar][ARP_LINE[e]] + 12)
    return out


def tables():
    """Lists of (offset in the synth's page, value) ending in $FF, then lo
    and hi pointer tables for each kind, OPL's entries before PSG's."""
    kinds = {
        "init": [opl_init(), psg_init()],
        "step": ([step(opl_note, s) for s in range(STEPS)] +
                 [step(psg_note, s) for s in range(STEPS)]),
        "chord": ([chord(opl_note, b) for b in range(4)] +
                  [chord(psg_note, b) for b in range(4)]),
    }
    blob, at = bytearray(), {}
    for kind, lists in kinds.items():
        at[kind] = []
        for lst in lists:
            assert len(lst) < 256
            at[kind].append(TABLES + len(blob))
            blob += bytes(lst) + b"\xff"
    ptr = {}
    for kind, addrs in at.items():
        ptr[kind] = (TABLES + len(blob), TABLES + len(blob) + len(addrs))
        blob += bytes(a & 0xFF for a in addrs)
        blob += bytes(a >> 8 for a in addrs)
    assert TABLES + len(blob) <= PCM_SRC, f"tables end at ${TABLES + len(blob):04X}"
    return bytes(blob), ptr


def program(ptr):
    p = Asm()
    loop, exit_, key_done = p.local("loop"), p.local("exit"), p.local("key")

    p.store(RW0_STEP, 1)
    p.say("\r\nPCM drums + OPL2/PSG music. Space switches, Q exits.\r\n")

    p.lda_imm(0)
    for zp in (ZP_WPH, ZP_SRC, ZP_LRPL, ZP_LRPH, ZP_POSL, ZP_POSH, ZP_STEP,
               ZP_MODE):
        p.sta_zp(zp)
    p.store(ZP_SRC + 1, PCM_SRC >> 8)
    p.jsr_abs("synth_start")

    for i, v in enumerate((0, 0, 0x03, RING_LOG2, RATE & 0xFF, RATE >> 8, 0, 0)):
        p.poke(XPCM + i, v)
    p.xreg(0, 1, 2, XPCM)
    p.jsr_abs("seq_step")

    p.symbol(loop)
    p.bit_abs(RIA_READY)
    p.bvc(key_done)
    p.lda_abs(RIA_RX)
    p.ora_imm(0x20)  # q and Q alike; a space stays a space
    p.cmp_imm(ord("q"))
    with p.branch("bne"):
        p.jmp_abs(exit_)
    p.cmp_imm(ord(" "))
    p.bne(key_done)
    p.jsr_abs("switch")
    p.symbol(key_done)
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
    p.jsr_abs("seq_step")
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

    def list_at(kind):
        """X indexes the kind's pointer tables; run that list."""
        lo, hi = ptr[kind]
        p.lda_abx(lo)
        p.sta_zp(ZP_PTR)
        p.lda_abx(hi)
        p.sta_zp(ZP_PTR + 1)

    p.symbol("seq_step")
    p.lda_zp(ZP_STEP)
    p.clc()
    p.adc_zp(ZP_MODE)
    p.tax()
    list_at("step")
    p.jsr_abs("run_list")
    p.lda_zp(ZP_STEP)
    p.clc()
    p.adc_imm(1)
    p.and_imm(STEPS - 1)
    p.sta_zp(ZP_STEP)
    p.rts()

    # Taking one synth parks the other. The chord of the bar being played
    # is struck again, since the new synth would hold no pad until the next
    # bar otherwise.
    p.symbol("switch")
    p.lda_zp(ZP_MODE)
    p.eor_imm(STEPS)
    p.sta_zp(ZP_MODE)
    p.jsr_abs("synth_start")
    p.lda_zp(ZP_STEP)
    p.sec()
    p.sbc_imm(1)
    p.and_imm(STEPS - 1)
    p.lsr_a()
    p.lsr_a()
    p.lsr_a()
    p.sta_zp(ZP_TMP)
    p.lda_zp(ZP_MODE)
    p.lsr_a()
    p.lsr_a()
    p.lsr_a()
    p.clc()
    p.adc_zp(ZP_TMP)
    p.tax()
    list_at("chord")
    p.jmp_abs("run_list")

    p.symbol("synth_start")
    p.lda_zp(ZP_MODE)
    p.bne("synth_start.psg")
    p.xreg(0, 1, 1, XOPL)
    p.store(ZP_PAGE, XOPL >> 8)
    p.say("OPL2\r\n")
    p.jmp_abs("synth_start.init")
    p.symbol("synth_start.psg")
    p.xreg(0, 1, 0, XPSG)
    p.store(ZP_PAGE, XPSG >> 8)
    p.say("PSG\r\n")
    p.symbol("synth_start.init")
    p.lda_zp(ZP_MODE)
    for _ in range(5):
        p.lsr_a()
    p.tax()
    list_at("init")
    p.jmp_abs("run_list")

    p.symbol("run_list")
    p.ldy_imm(0)
    p.symbol("run_list.next")
    p.lda_izy(ZP_PTR)
    p.cmp_imm(0xFF)
    with p.branch("bne"):
        p.rts()
    p.sta_abs(RW0_ADDR)
    p.lda_zp(ZP_PAGE)
    p.sta_abs(RW0_ADDR + 1)
    p.iny()
    p.lda_izy(ZP_PTR)
    p.sta_abs(RW0_DATA)
    p.iny()
    p.jmp_abs("run_list.next")

    p.use(putc)
    return p


def prog():
    blob, ptr = tables()
    p = program(ptr)
    assert p.here() <= TABLES, f"code ends at ${p.here():04X}"
    p.rom.add_binary_data(blob, TABLES)
    p.rom.add_binary_data(drums(), PCM_SRC)
    return p


ROMS = {"pcm+opl_or_psg": prog}
main(globals())
