/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Both audio engines end to end, in the fewest frames that can prove it:
 * a program asks for the device with xreg, pours its registers into the
 * XRAM page through RW0, and the machine makes a noise. Nothing here
 * checks a waveform — psg's lockstep does that for the PSG, and the
 * OPL2 is a different implementation from emu8950 with no agreement to
 * hold it to. What these check is every link between a 6502 store and a
 * sample leaving the machine.
 *
 * This replaces a furelise playthrough that took eighty frames to reach
 * its first note and, for all that, only ever exercised the PSG. The
 * OPL2 reached hardware silent behind exactly that gap: the PSG re-reads
 * its whole config from XRAM every tick and needs the snoop only for
 * gate edges, so a working PSG says nothing about a snoop's ability to
 * carry register data — and the OPL has nothing else.
 *
 * Energy is the measurement, and peak with it. A sum alone passes on an
 * average deviation of one count, which is what a voice scaled wrong
 * looks like, and that too has already shipped once.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "tb_quiet.h"
#include "tb_stage.h"
#include "tb_tcm.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <vector>

static Vrp6502 *dut;
/* Half clk_sys, rising with it: the PLL's shape, not a divider's. */
static bool rv_phase;
static std::vector<uint8_t> rom;

static long g_valids;
static long g_energy;
static int g_peak;

static bool load_firmware(const char *path)
{
    auto *r = dut->rootp;
    return tb_load_tcm(r->rp6502__DOT__rv__DOT__tcm0,
                       r->rp6502__DOT__rv__DOT__tcm1,
                       r->rp6502__DOT__rv__DOT__tcm2,
                       r->rp6502__DOT__rv__DOT__tcm3, path);
}

static void clock_cycle()
{
    uint32_t a = dut->rp6502_stage_addr;
    dut->stage_rdata = tb_stage(rom, a);
    rv_phase = !rv_phase;
    dut->clk_rv = rv_phase;
    dut->clk_sys = 1;
    dut->eval();
    dut->clk_rv = 0;
    dut->clk_sys = 0;
    dut->eval();
    if (dut->rp6502_aud_valid)
    {
        g_valids++;
        int dl = (int)dut->rp6502_aud_l - 512;
        int dr = (int)dut->rp6502_aud_r - 512;
        if (dl < 0)
            dl = -dl;
        if (dr < 0)
            dr = -dr;
        g_energy += dl + dr;
        if (dl > g_peak)
            g_peak = dl;
        if (dr > g_peak)
            g_peak = dr;
    }
}

static void run_frame()
{
    while (dut->rp6502_scanline != 524)
        clock_cycle();
    while (dut->rp6502_scanline != 0)
        clock_cycle();
}

static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--)
    {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static void rom_record(std::vector<uint8_t> &r, uint32_t addr,
                       const uint8_t *data, size_t len)
{
    char line[64];
    snprintf(line, sizeof line, "$%05X $%zX $%08X\n", addr, len,
             crc32_buf(data, len));
    r.insert(r.end(), line, line + strlen(line));
    r.insert(r.end(), data, data + len);
}

/* A 6502 program builder: the API call shape from test_xreg, plus RW0
 * stores, which is all either engine needs. */
struct Prog
{
    std::vector<uint8_t> p;

    void lda(uint8_t v) { p.insert(p.end(), {0xA9, v}); }
    void sta(uint16_t a)
    {
        p.insert(p.end(), {0x8D, (uint8_t)a, (uint8_t)(a >> 8)});
    }
    void push(uint8_t v) { lda(v); sta(0xFFEC); }
    void pushw(uint16_t w) { push((uint8_t)(w >> 8)); push((uint8_t)w); }
    /* op 0x01 is xreg; the trampoline at $FFF1 waits for the answer. */
    void op1()
    {
        lda(0x01);
        sta(0xFFEF);
        p.insert(p.end(), {0x20, 0xF1, 0xFF});
    }
    void xreg(uint8_t dev, uint8_t ch, uint8_t addr, uint16_t word)
    {
        push(dev);
        push(ch);
        push(addr);
        pushw(word);
        op1();
    }
    /* One XRAM byte through RW0: point ADDR0, then store the value.
     * STEP0 stays zero so the address is whatever was last written and
     * the order of these is the order the snoop sees. */
    void poke(uint16_t addr, uint8_t val)
    {
        lda((uint8_t)addr);
        sta(0xFFE6);
        lda((uint8_t)(addr >> 8));
        sta(0xFFE7);
        lda(val);
        sta(0xFFE4);
    }
    /* About 1,280 cycles, which at 8 MHz PHI2 is 160 us. The PSG applies
     * its pointer at a sample boundary rather than on the write, so for
     * one sample period after xreg returns the device is still off and a
     * gate edge written into that window is never snooped. A tracker
     * never notices because it gates again every row; a test that pokes
     * seven bytes and stops would. */
    void settle()
    {
        p.insert(p.end(), {0xA2, 0x00});       /* ldx #0    */
        p.insert(p.end(), {0xCA});             /* dex       */
        p.insert(p.end(), {0xD0, 0xFD});       /* bne -3    */
    }
    void spin() { p.insert(p.end(), {0x4C, 0, 0}); } /* jmp self, patched */
};

static bool build_and_reset(Prog &prog, uint16_t org)
{
    /* The spin lands on itself, so the program holds while the engine
     * runs rather than falling into whatever follows. */
    uint16_t here = (uint16_t)(org + prog.p.size() - 2);
    prog.p[prog.p.size() - 2] = (uint8_t)here;
    prog.p[prog.p.size() - 1] = (uint8_t)(here >> 8);

    const uint8_t vectors[2] = {(uint8_t)org, (uint8_t)(org >> 8)};
    const char magic[] = "#!RP6502\n";
    rom.clear();
    rom.insert(rom.end(), magic, magic + strlen(magic));
    rom_record(rom, org, prog.p.data(), prog.p.size());
    rom_record(rom, 0xFFFC, vectors, sizeof vectors);

    if (!load_firmware(SW_BIN))
        return false;
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
        clock_cycle();
    dut->rst_n = 1;
    dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();
    g_valids = g_energy = 0;
    g_peak = 0;
    return true;
}

/* Frames until the engine is heard, or -1. Two is the budget; the load
 * and the program's own setup eat most of the first. */
static int frames_to_sound(int limit)
{
    for (int i = 0; i < limit; i++)
    {
        g_energy = 0;
        g_peak = 0;
        run_frame();
        if (g_peak > 0)
            return i;
    }
    return -1;
}

UTEST(aud, psg_makes_a_noise)
{
    Prog prog;
    const uint16_t page = 0x8000;
    prog.xreg(0, 1, 0, page);
    prog.settle();
    /* One channel: 440 Hz (the engine divides by three), half duty,
     * loudest volume and the fastest attack. The volume nibble is an
     * index into psg_vol_table, which runs 256 down to 0 — it attenuates,
     * so zero is loud and fifteen is silence. The gate in byte 6 goes
     * last, because that edge is what starts the note. */
    prog.poke(page + 0, 0x28); /* freq low  */
    prog.poke(page + 1, 0x05); /* freq high */
    prog.poke(page + 2, 0x80); /* duty      */
    prog.poke(page + 3, 0x00); /* loudest, fastest attack */
    prog.poke(page + 4, 0x00); /* sustain at the same level */
    prog.poke(page + 5, 0x00); /* square, release */
    prog.poke(page + 6, 0x01); /* pan centre, gate on */
    prog.spin();

    ASSERT_TRUE(build_and_reset(prog, 0x0300));
    int at = frames_to_sound(8);
    ASSERT_NE(at, -1);
    ASSERT_LT(at, 3);
    /* Loud, not merely moving. */
    ASSERT_GT(g_peak, 32);
    ASSERT_GT(g_valids, (long)0);
}

UTEST(aud, opl_makes_a_noise)
{
    Prog prog;
    const uint16_t page = 0xF000;
    prog.xreg(0, 1, 1, page);
    /* Channel 0 of a YM3812: modulator is slot 0, carrier slot 3. The
     * key-on in 0xB0 goes last for the same reason the PSG's gate does. */
    prog.poke(page + 0x20, 0x01); /* modulator mult 1 */
    prog.poke(page + 0x23, 0x01); /* carrier mult 1   */
    prog.poke(page + 0x40, 0x10); /* modulator level  */
    prog.poke(page + 0x43, 0x00); /* carrier full     */
    prog.poke(page + 0x60, 0xF0); /* fast attack      */
    prog.poke(page + 0x63, 0xF0);
    prog.poke(page + 0x80, 0x77); /* sustain, release */
    prog.poke(page + 0x83, 0x77);
    prog.poke(page + 0xC0, 0x0E); /* feedback, both operators out */
    prog.poke(page + 0xA0, 0x98); /* f-number low */
    prog.poke(page + 0xB0, 0x31); /* key on, block 4, f-number high */
    prog.spin();

    ASSERT_TRUE(build_and_reset(prog, 0x0300));
    int at = frames_to_sound(8);
    ASSERT_NE(at, -1);
    ASSERT_LT(at, 3);
    ASSERT_GT(g_peak, 32);
    ASSERT_GT(g_valids, (long)0);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vrp6502;
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->rst_n = 0;
    dut->eval();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
