#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

import struct
import wave

import rp6502
import math

from rp6502_asm import (API_A, API_ERRNO, API_OP, API_X, OP_ATTR_GET,
                        OP_ATTR_SET, OP_CLOSE, OP_ERRNO_OPT, OP_EXIT, OP_LSEEK,
                        OP_OPEN, OP_SYNCFS, OP_WRITE_XRAM, OP_WRITE_XSTACK,
                        O_WRONLY, RIA_TX, RW0_ADDR, RW0_DATA, RW0_STEP,
                        RW1_ADDR, RW1_DATA, RW1_STEP, SEEK_CUR, SEEK_END, SEEK_SET, VIA_ACR, VIA_IFR,
                        VIA_T1_HI, VIA_T1_LO, VIA_T1L_HI, VIA_T1L_LO, XSTACK,
                        Asm)
from rp6502_rom import main, rom

PSG = 0x8000
GO = 0x0200
ATTR_PCM_UNDERRUNS = 0x0E
ATTR_OPL_OVERRUNS = 0x0F
# opl_overruns_prog's and pcm_dry_prog's answers: A and X of each call, the
# errno of the last, then 1 once all are in.
GOT = 0x0210


class Prog(Asm):
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
    # Each pair of delays lasts about 656,000 cycles, which is about five
    # frames at the default 8 MHz, so a test can measure whole frames
    # between the steps.
    p.delay(255)
    p.delay(255)
    p.poke(page + 6, 0x01)
    p.delay(255)
    p.delay(255)
    p.poke(page + 0, 0x28)
    p.poke(page + 1, 0x05)
    p.poke(page + 2, 0x80)
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
    frame at the default 8 MHz, before the program exits, so the frame the
    test records first holds it."""
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
    """The page is cleared through RW0, which queues the 135 offsets that are
    registers, then the eleven writes of a note, which the queue of 256 also
    holds. Then register 1 is written with zero 2,040 times, which overflows
    the queue and changes nothing a machine without one plays."""
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
    p.spin()
    return p


def opl_spread_prog():
    """300 writes to register 40, 445 cycles apart, about one frame at the
    default 8 MHz. The step of zero holds RW0 on the register."""
    p = Prog()
    page = 0xF000
    p.xreg(0, 1, 1, page)
    p.store(RW0_STEP, 0x00)
    p.store(RW0_ADDR, 0x40)
    p.store(RW0_ADDR + 1, page >> 8)
    block, write, wait = p.local("block"), p.local("write"), p.local("wait")
    p.store(0x10, 2)
    p.symbol(block)
    p.ldx_imm(150)
    p.symbol(write)
    p.stx_abs(RW0_DATA)
    p.ldy_imm(87)
    p.symbol(wait)
    p.dey()
    p.bne(wait)
    p.dex()
    p.bne(write)
    p.dec_zp(0x10)
    p.bne(block)
    p.spin()
    return p


def opl_flat_prog():
    """768 writes to register 40 as fast as a loop makes them, 9 cycles
    apart, far past what the queue holds."""
    p = Prog()
    page = 0xF000
    p.xreg(0, 1, 1, page)
    p.store(RW0_STEP, 0x00)
    p.store(RW0_ADDR, 0x40)
    p.store(RW0_ADDR + 1, page >> 8)
    loop = p.local("flat")
    p.ldx_imm(0)
    p.ldy_imm(3)
    p.symbol(loop)
    p.stx_abs(RW0_DATA)
    p.inx()
    p.bne(loop)
    p.dey()
    p.bne(loop)
    p.spin()
    return p


def opl_overruns_prog():
    """300 writes to register 40, 4 cycles apart, more than the queue of 256
    holds. Then the overrun count is read twice and written once through the
    attribute."""
    p = Prog()
    page = 0xF000
    p.xreg(0, 1, 1, page)
    p.store(RW0_STEP, 0x00)
    p.store(RW0_ADDR, 0x40)
    p.store(RW0_ADDR + 1, page >> 8)
    for _ in range(300):
        p.sta_abs(RW0_DATA)
    p.store(RW0_STEP, 0x01)
    attr_answers(p, ATTR_OPL_OVERRUNS)
    p.spin()
    return p


def attr_answers(p, attr):
    """The attribute read twice and written once, into GOT."""
    p.call_a(OP_ATTR_GET, attr)
    for i, reg in enumerate((API_A, API_X)):
        p.lda_abs(reg)
        p.sta_abs(GOT + i)
    p.call_a(OP_ATTR_GET, attr)
    for i, reg in enumerate((API_A, API_X)):
        p.lda_abs(reg)
        p.sta_abs(GOT + 2 + i)
    p.pushl(0)
    p.call_a(OP_ATTR_SET, attr)
    for i, reg in enumerate((API_A, API_X, API_ERRNO)):
        p.lda_abs(reg)
        p.sta_abs(GOT + 4 + i)
    p.store(GOT + 7, 1)


# pcm_dry_prog's 10 ms of silence.
PCM_DRY_LEN = 480


def pcm_dry_prog():
    """10 ms at 48000 Hz, 8-bit mono, which play out within a frame. Three
    frames later the underrun count is read twice and written once through
    the attribute."""
    p = Prog()
    p.rom.add_binary_data(bytes([128]) * PCM_DRY_LEN, 0x10000 + PCM_TABLE)
    fd, frame = 0x12, 0x13
    p.push_str("PCM:48000,8,1")
    p.call_a(OP_OPEN, O_WRONLY)
    p.sta_zp(fd)
    for v in (PCM_TABLE, PCM_DRY_LEN):
        p.lda_imm(v >> 8)
        p.sta_abs(XSTACK)
        p.lda_imm(v & 0xFF)
        p.sta_abs(XSTACK)
    p.lda_zp(fd)
    p.sta_abs(API_A)
    p.call(OP_WRITE_XRAM)
    p.ldx_imm(3)
    p.symbol("frame")
    p.lda_abs(VSYNC)
    p.sta_zp(frame)
    p.symbol("wait")
    p.lda_abs(VSYNC)
    p.cmp_zp(frame)
    p.beq("wait")
    p.dex()
    p.bne("frame")
    attr_answers(p, ATTR_PCM_UNDERRUNS)
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



# The recordings are 48 kHz, 800 samples to a frame. One PSG voice at full
# volume peaks near 16127, and one OPL2 voice at a real YM3812's level near
# 4088, both over LOUD. BELL is a level no silence reaches.
FRAME = 800
LOUD = 2048
BELL = 32

# 97 whole cycles of 1050 Hz at 22050 Hz, 21 samples each, in XRAM.
PCM_RATE = 22050
PCM_HZ = 1050
PCM_TABLE = 0x1000
PCM_LEN = 97 * 21 * 2
PCM_PEAK = 16000
VSYNC = 0xFFE3


def pcm_prog():
    """PCM:22050,16,1 fed once a frame from a looped table in XRAM, each
    write picking up where the buffer last stopped taking."""
    p = Prog()
    table = b"".join(
        int(round(PCM_PEAK * math.sin(2 * math.pi * i / 21))).to_bytes(2, "little", signed=True)
        for i in range(PCM_LEN // 2))
    p.rom.add_binary_data(table, 0x10000 + PCM_TABLE)
    pos, fd, frame = 0x10, 0x12, 0x13
    p.push_str(f"PCM:{PCM_RATE},16,1")
    p.call_a(OP_OPEN, O_WRONLY)
    p.sta_zp(fd)
    p.lda_imm(0)
    p.sta_zp(pos)
    p.sta_zp(pos + 1)
    p.symbol("frame")
    p.lda_abs(VSYNC)
    p.sta_zp(frame)
    p.symbol("wait")
    p.lda_abs(VSYNC)
    p.cmp_zp(frame)
    p.beq("wait")
    p.symbol("fill")
    # The address, PCM_TABLE + pos, then the count, PCM_LEN - pos, each
    # pushed high byte first.
    p.clc()
    p.lda_zp(pos)
    p.adc_imm(PCM_TABLE & 0xFF)
    p.tay()
    p.lda_zp(pos + 1)
    p.adc_imm(PCM_TABLE >> 8)
    p.sta_abs(XSTACK)
    p.sty_abs(XSTACK)
    p.sec()
    p.lda_imm(PCM_LEN & 0xFF)
    p.sbc_zp(pos)
    p.tay()
    p.lda_imm(PCM_LEN >> 8)
    p.sbc_zp(pos + 1)
    p.sta_abs(XSTACK)
    p.sty_abs(XSTACK)
    p.lda_zp(fd)
    p.sta_abs(API_A)
    p.call(OP_WRITE_XRAM)
    # pos += what the buffer took, the count in A and X.
    p.clc()
    p.adc_zp(pos)
    p.sta_zp(pos)
    p.txa()
    p.adc_zp(pos + 1)
    p.sta_zp(pos + 1)
    p.lda_zp(pos)
    p.cmp_imm(PCM_LEN & 0xFF)
    with p.branch("bne"):
        p.lda_zp(pos + 1)
        p.cmp_imm(PCM_LEN >> 8)
        with p.branch("bne"):
            p.lda_imm(0)
            p.sta_zp(pos)
            p.sta_zp(pos + 1)
            p.jmp_abs("fill")
    p.jmp_abs("frame")
    return p


# aud_sst_prog's PCM table in XRAM, 768 stereo frames of two tones.
SST_PCM_TABLE = 0x2000
SST_PCM_LEN = 3072
# The loop's VIA T1 period, 256 us at the default 8 MHz.
SST_T1 = 0x0800
SST_OPL = 0xF000


def aud_sst_prog():
    """Every engine at once, for a savestate taken mid-note: a PSG tone and
    a noise voice, the OPL in rhythm mode with feedback, vibrato and
    tremolo, and the bell, rung well before PCM 22050 Hz 16-bit stereo is
    opened and filled three quarters full. Then W on the console, and a
    loop that makes no API call: each VIA T1 period it writes one entry of
    a table through RW0, PSG gates and frequencies and OPL keys and drums,
    and every sixteenth period a burst of 24 into the OPL's queue."""
    p = Prog()
    frames = []
    for i in range(SST_PCM_LEN // 4):
        left = int(round(12000 * math.sin(2 * math.pi * i / 21)))
        right = int(round(9000 * math.sin(2 * math.pi * i / 34)))
        frames.append(left.to_bytes(2, "little", signed=True)
                      + right.to_bytes(2, "little", signed=True))
    p.rom.add_binary_data(b"".join(frames), 0x10000 + SST_PCM_TABLE)
    entries = []
    for i in range(32):
        entries += [
            (PSG + 0, 0x28 + 8 * (i % 5)),
            (PSG + 6, 0x01 if i % 3 else 0x00),
            (SST_OPL + 0xB0, 0x31 if i % 2 else 0x11),
            (SST_OPL + 0xBD, 0xE0 | (i & 0x1F)),
            (PSG + 8, 0x40 + 16 * (i % 7)),
            (SST_OPL + 0xA0, 0x80 + 4 * (i % 9)),
        ]
    entries = entries[:255]
    fd, count = 0x10, 0x11
    p.jmp_abs("main")
    p.symbol("tab_lo")
    p.data(bytes(a & 0xFF for a, _ in entries))
    p.symbol("tab_hi")
    p.data(bytes(a >> 8 for a, _ in entries))
    p.symbol("tab_val")
    p.data(bytes(v for _, v in entries))
    p.symbol("main")

    p.xreg(0, 1, 0, PSG)
    for at, val in ((0, 0x28), (1, 0x05), (2, 0x80), (3, 0x00), (4, 0x00),
                    (5, 0x10), (6, 0x01),
                    (8, 0x00), (9, 0x30), (10, 0xC0), (11, 0x20),
                    (12, 0x20), (13, 0x43), (14, 0x41)):
        p.poke(PSG + at, val)
    p.xreg(0, 1, 1, SST_OPL)
    for reg, val in (
        (0x01, 0x20), (0x20, 0xE1), (0x23, 0xC1), (0x40, 0x10), (0x43, 0x00),
        (0x60, 0xF4), (0x63, 0xF4), (0x80, 0x57), (0x83, 0x57), (0xE0, 0x00),
        (0xE3, 0x02), (0xC0, 0x0E), (0xA0, 0x98), (0xB0, 0x31),
        (0x30, 0x01), (0x31, 0x02), (0x32, 0x01), (0x33, 0x01), (0x34, 0x01),
        (0x35, 0x01), (0x70, 0xF6), (0x71, 0xF6), (0x72, 0xF6), (0x73, 0xF6),
        (0x74, 0xF6), (0x75, 0xF6), (0x90, 0x46), (0x91, 0x46), (0x92, 0x46),
        (0x93, 0x46), (0x94, 0x46), (0x95, 0x46), (0xA6, 0x50), (0xB6, 0x09),
        (0xA7, 0x60), (0xB7, 0x09), (0xA8, 0x70), (0xB8, 0x09), (0xC6, 0x0B),
        (0xBD, 0xFF),
    ):
        p.poke(SST_OPL + reg, val)

    # The bell's 20 ms release is past before the delay ends.
    p.lda_imm(0x07)
    p.putc_a()
    p.delay(255)

    p.push_str("PCM:22050,16,2")
    p.call_a(OP_OPEN, O_WRONLY)
    p.sta_zp(fd)
    p.pushw(SST_PCM_TABLE)
    p.pushw(SST_PCM_LEN)
    p.lda_zp(fd)
    p.sta_abs(API_A)
    p.call(OP_WRITE_XRAM)

    p.lda_imm(ord("W"))
    p.putc_a()

    p.store(RW0_STEP, 0x00)
    p.store(VIA_T1L_LO, SST_T1 & 0xFF)
    p.store(VIA_T1L_HI, SST_T1 >> 8)
    p.store(VIA_T1_LO, SST_T1 & 0xFF)
    p.store(VIA_T1_HI, SST_T1 >> 8)
    p.store(VIA_ACR, 0x40)
    p.store(count, 0)
    p.ldx_imm(0)
    p.symbol("period")
    p.lda_abs(VIA_IFR)
    p.and_imm(0x40)
    p.beq("period")
    p.lda_abs(VIA_T1_LO)
    p.lda_abx("tab_lo")
    p.sta_abs(RW0_ADDR)
    p.lda_abx("tab_hi")
    p.sta_abs(RW0_ADDR + 1)
    p.lda_abx("tab_val")
    p.sta_abs(RW0_DATA)
    p.inx()
    p.cpx_imm(len(entries))
    with p.branch("bne"):
        p.ldx_imm(0)
    p.inc_zp(count)
    p.lda_zp(count)
    p.and_imm(0x0F)
    p.bne("period")
    p.store(RW0_ADDR, 0xA1)
    p.store(RW0_ADDR + 1, SST_OPL >> 8)
    for k in range(24):
        p.store(RW0_DATA, 0x60 + k)
    p.jmp_abs("period")
    return p


def aud_snoop_prog():
    """The PSG and the OPL playing, then W on the console and RW0 and RW1
    writing them back to back, so a stop often finds a 6502 write in
    flight: RW0 into voice 0's frequency once in eight, RW1 into the OPL's
    queue, which overflows, the rest."""
    p = Prog()
    p.xreg(0, 1, 0, PSG)
    for at, val in ((0, 0x28), (1, 0x05), (2, 0x80), (3, 0x00), (4, 0x00),
                    (5, 0x30), (6, 0x01)):
        p.poke(PSG + at, val)
    p.xreg(0, 1, 1, SST_OPL)
    for reg, val in (
        (0x20, 0x21), (0x23, 0x01), (0x40, 0x10), (0x43, 0x00), (0x60, 0xF4),
        (0x63, 0xF4), (0x80, 0x57), (0x83, 0x57), (0xC0, 0x0A), (0xA0, 0x98),
        (0xB0, 0x31),
    ):
        p.poke(SST_OPL + reg, val)
    p.store(RW0_STEP, 0x00)
    p.store(RW0_ADDR, PSG & 0xFF)
    p.store(RW0_ADDR + 1, PSG >> 8)
    p.store(RW1_STEP, 0x00)
    p.store(RW1_ADDR, 0xA0)
    p.store(RW1_ADDR + 1, SST_OPL >> 8)
    p.lda_imm(ord("W"))
    p.putc_a()
    p.ldx_imm(0)
    p.symbol("flood")
    p.stx_abs(RW0_DATA)
    for _ in range(7):
        p.stx_abs(RW1_DATA)
    p.inx()
    p.jmp_abs("flood")
    return p


# pcm_rules_prog's answers, a word each from RULES, then 1 at RULES_DONE.
RULES = 0x0220
(R_OPEN_48001, R_ERRNO_48001, R_OPEN_48000, R_OPEN_BUSY, R_ERRNO_BUSY,
 R_END, R_WRITE, R_CUR0, R_CUR1, R_CUR2, R_SYNC, R_CUR_SYNCED, R_REFILL,
 R_SET, R_CUR_SET, R_CLOSE, R_REOPEN, R_WRITE_800, R_VSYNC0, R_CLOSE_800,
 R_VSYNC1, R_WRITE_EXIT) = range(22)
RULES_DONE = RULES + 2 * 22
ERRNO_OPT_LLVM = 2
EBUSY = 16
EINVAL = 22
# The bytes 48000 Hz 16-bit stereo plays in delay(25), 32,150 cycles at the
# default 8 MHz.
CUR_FALL = 771


def pcm_rules_prog():
    """The PCM: device's rules as a program sees them. 48001 Hz is refused,
    48000 Hz opens and a second open is refused. A write of 8192 bytes takes
    the 4096 that fit, SEEK_CUR falls as they play and fsync waits until they
    have. SEEK_SET empties the buffer. 800 bytes at 8000 Hz play for 100 ms,
    six frames, which their close waits out. Then the program exits with
    4096 bytes at 8000 Hz waiting, half a second, and the exit does not wait
    for them."""
    p = Prog()
    fd = 0x12
    p.store(API_A, ERRNO_OPT_LLVM)
    p.call(OP_ERRNO_OPT)

    def keep(i):
        p.sta_abs(RULES + 2 * i)
        p.stx_abs(RULES + 2 * i + 1)

    def keep_errno(i):
        p.lda_abs(API_ERRNO)
        p.ldx_abs(API_ERRNO + 1)
        keep(i)

    def open_pcm(args):
        p.push_str(f"PCM:{args}")
        p.call_a(OP_OPEN, O_WRONLY)

    def on_fd(op):
        p.lda_zp(fd)
        p.sta_abs(API_A)
        p.call(op)

    def seek(whence):
        p.pushl(0)
        p.push(whence)
        on_fd(OP_LSEEK)

    def write(size):
        p.pushw(PCM_TABLE)
        p.pushw(size)
        on_fd(OP_WRITE_XRAM)

    def vsync(i):
        p.lda_abs(VSYNC)
        p.ldx_imm(0)
        keep(i)

    open_pcm("48001,16,2")
    keep(R_OPEN_48001)
    keep_errno(R_ERRNO_48001)
    open_pcm("48000,16,2")
    keep(R_OPEN_48000)
    p.sta_zp(fd)
    open_pcm("8000,8,1")
    keep(R_OPEN_BUSY)
    keep_errno(R_ERRNO_BUSY)
    seek(SEEK_END)
    keep(R_END)
    write(8192)
    keep(R_WRITE)
    seek(SEEK_CUR)
    keep(R_CUR0)
    p.delay(25)
    seek(SEEK_CUR)
    keep(R_CUR1)
    p.delay(25)
    seek(SEEK_CUR)
    keep(R_CUR2)
    on_fd(OP_SYNCFS)
    keep(R_SYNC)
    seek(SEEK_CUR)
    keep(R_CUR_SYNCED)
    write(4096)
    keep(R_REFILL)
    seek(SEEK_SET)
    keep(R_SET)
    seek(SEEK_CUR)
    keep(R_CUR_SET)
    on_fd(OP_CLOSE)
    keep(R_CLOSE)
    open_pcm("8000,8,1")
    keep(R_REOPEN)
    p.sta_zp(fd)
    write(800)
    keep(R_WRITE_800)
    vsync(R_VSYNC0)
    on_fd(OP_CLOSE)
    keep(R_CLOSE_800)
    vsync(R_VSYNC1)
    open_pcm("8000,8,1")
    p.sta_zp(fd)
    write(4096)
    keep(R_WRITE_EXIT)
    p.store(RULES_DONE, 1)
    p.store(API_A, 0)
    p.store(API_X, 0)
    p.call(OP_EXIT)
    p.spin()
    return p


# pcm_order_prog's writes, from XRAM and then from the xstack. Each count
# written is kept as a word from ORDER, then 1 at ORDER_DONE.
ORDER = 0x0250
ORDER_XRAM = [1, 3, 2, 5, 7, 4, 9, 6, 11, 13, 8, 1, 1, 1, 17, 30, 2, 3]
ORDER_XSTACK = [1, 2, 3, 5, 7, 4, 6, 1, 3]
ORDER_DONE = ORDER + 2 * (len(ORDER_XRAM) + len(ORDER_XSTACK))


def order_byte(i):
    """Byte i of pcm_order_prog's stream."""
    return (i * 7 + 3) & 0xFF


def pcm_order_prog():
    """PCM:1,8,1 takes a byte a second, so the buffer still holds every byte
    of these writes, which start at every alignment and leave every
    remainder. The stream is order_byte(0) onward."""
    p = Prog()
    n = sum(ORDER_XRAM)
    p.rom.add_binary_data(bytes(order_byte(i) for i in range(n)), 0x10000 + PCM_TABLE)
    fd = 0x12
    p.push_str("PCM:1,8,1")
    p.call_a(OP_OPEN, O_WRONLY)
    p.sta_zp(fd)
    keep = ORDER

    def write(op):
        nonlocal keep
        p.lda_zp(fd)
        p.sta_abs(API_A)
        p.call(op)
        p.sta_abs(keep)
        p.stx_abs(keep + 1)
        keep += 2

    at = 0
    for size in ORDER_XRAM:
        p.pushw(PCM_TABLE + at)
        p.pushw(size)
        write(OP_WRITE_XRAM)
        at += size
    for size in ORDER_XSTACK:
        for i in reversed(range(at, at + size)):
            p.push(order_byte(i))
        write(OP_WRITE_XSTACK)
        at += size
    p.store(ORDER_DONE, 1)
    p.spin()
    return p


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


def opl_drops(path):
    """The OPL2's drop count from a savestate. A software machine decides each
    write as the 6502 makes it and keeps the count in the AUD_ chunk, whose
    id, version 5 and length come before its fields: the engines byte, two
    resamplers of 105 bytes and the 8-byte samples made come before it."""
    with open(path, "rb") as f:
        blob = f.read()
    at = blob.index(b"AUD_\x00\x05") + 10 + 1 + 2 * 105 + 8
    return int.from_bytes(blob[at:at + 4], "big")


def test_psg():
    """The note starts within three frames, loud, and the twelve frames after
    its first are 440 Hz within a tenth. Then the gate byte is cleared twice.
    A plain poke changes XRAM only, and the PSG, which takes its registers
    only as the RIA writes them, plays on. xbus: writes it the way the RIA
    does, and the note is released."""
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
    """The clear and the note fit the queue, so the note sounds, and the burst
    after them is counted as drops."""
    record("opl_init", 'wav "init.wav" 3', 'state save "init.sst"')
    p = peaks(mono("init.wav"))
    at = onset(p)
    assert at >= 0 and p[at] > LOUD, f"no loud note in frames 0-2: peaks {p}"
    drops = opl_drops("init.sst")
    assert 0 < drops <= 2040, f"{drops} drops"


def test_psg_pre():
    """The block is programmed, gate and all, before xreg names it. The map
    starts the PSG's registers at zero whatever XRAM holds, so the gate
    struck about five frames later plays no tone, and the tone starts only
    when the program writes the frequency and duty about five frames after
    that."""
    record("psg_pre", 'wav "pre.wav" 20')
    m = mono("pre.wav")
    p = peaks(m)
    assert not any(p[1:4]), f"sound in frames 1-3: peaks {p[1:4]}"
    tone = [crossings(m[i:i + FRAME]) for i in range(0, len(m), FRAME)]
    assert not any(tone[:9]), f"a tone in frames 0-8: swings {tone[:9]}"
    at = next((i for i in range(9, len(tone)) if tone[i] > 10), -1)
    assert at >= 0 and p[at] > LOUD, \
        f"no tone in frames 9-19: swings {tone[9:]}, peaks {p[9:]}"


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


def test_pcm():
    """Ten frames of the tone, after it has had two to start, are 1050 Hz
    within a tenth and as loud as the table."""
    record("pcm", "run 2", 'wav "pcm.wav" 10')
    m = mono("pcm.wav")
    cross = crossings(m)
    hz = cross / 2 * 48000 / len(m)
    peak = max(abs(v) for v in m)
    assert abs(hz - PCM_HZ) < PCM_HZ / 10, f"{cross} crossings, {hz:.1f} Hz"
    assert abs(peak - PCM_PEAK) < PCM_PEAK * 0.02, f"peak {peak}"


def check_pcm_rules(w):
    """pcm_rules_prog's answers, as words, on any machine."""
    assert (w[R_OPEN_48001], w[R_ERRNO_48001]) == (0xFFFF, EINVAL), \
        f"open 48001: {w[R_OPEN_48001]:04X} errno {w[R_ERRNO_48001]}"
    assert w[R_OPEN_48000] < 0x8000, f"open 48000: {w[R_OPEN_48000]:04X}"
    assert (w[R_OPEN_BUSY], w[R_ERRNO_BUSY]) == (0xFFFF, EBUSY), \
        f"second open: {w[R_OPEN_BUSY]:04X} errno {w[R_ERRNO_BUSY]}"
    assert (w[R_END], w[R_WRITE]) == (4096, 4096), \
        f"SEEK_END {w[R_END]}, write took {w[R_WRITE]}"
    cur = w[R_CUR0:R_CUR2 + 1]
    assert 4096 >= cur[0] > cur[1] > cur[2] > 0 \
        and all(c % 4 == 0 for c in cur) \
        and all(abs(a - b - CUR_FALL) < CUR_FALL // 8
                for a, b in zip(cur, cur[1:])), f"SEEK_CUR {cur}"
    assert (w[R_SYNC], w[R_CUR_SYNCED]) == (0, 0), \
        f"fsync {w[R_SYNC]:04X}, SEEK_CUR after it {w[R_CUR_SYNCED]}"
    assert (w[R_REFILL], w[R_SET], w[R_CUR_SET]) == (4096, 0, 0), \
        f"write {w[R_REFILL]}, SEEK_SET {w[R_SET]}, SEEK_CUR {w[R_CUR_SET]}"
    assert w[R_CLOSE] == 0, f"close {w[R_CLOSE]:04X}"
    assert w[R_REOPEN] < 0x8000 and w[R_WRITE_800] == 800 \
        and w[R_CLOSE_800] == 0, \
        f"open {w[R_REOPEN]:04X}, write {w[R_WRITE_800]}, close {w[R_CLOSE_800]:04X}"
    frames = (w[R_VSYNC1] - w[R_VSYNC0]) & 0xFF
    assert 5 <= frames <= 7, f"the close of 100 ms took {frames} frames"
    assert w[R_WRITE_EXIT] == 4096, f"the last write took {w[R_WRITE_EXIT]}"


def test_pcm_rules():
    with rp6502.Emu(rom("pcm_rules")) as e:
        e.cmd("wait RULES_DONE 1 60")
        got = bytes.fromhex(e.cmd(f"dump RULES {RULES_DONE - RULES}"))
        e.cmd("wait exit 2")
        e.cmd("expect exit 0")
    check_pcm_rules([got[i] | got[i + 1] << 8 for i in range(0, len(got), 2)])


ROMS = {"psg": psg_prog, "psg_pre": psg_pre_prog, "opl": opl_prog,
        "opl_exit": opl_exit_prog, "opl_init": opl_init_prog,
        "opl_spread": opl_spread_prog, "opl_flat": opl_flat_prog,
        "opl_overruns": opl_overruns_prog, "pcm_dry": pcm_dry_prog,
        "bel": bel_prog, "bel_off": bel_off_prog, "opl_bel": opl_bel_prog,
        "pcm": pcm_prog, "pcm_rules": pcm_rules_prog,
        "pcm_order": pcm_order_prog, "aud_sst": aud_sst_prog,
        "aud_snoop": aud_snoop_prog}
TESTS = {"psg": test_psg, "psg_pre": test_psg_pre, "opl": test_opl,
         "opl_exit": test_opl_exit, "opl_init": test_opl_init,
         "bel": test_bel, "bel_off": test_bel_off, "opl_bel": test_opl_bel,
         "pcm": test_pcm, "pcm_rules": test_pcm_rules}
main(globals())
