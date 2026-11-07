/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * psg.c and psg.sv, the bell included as both models' ninth voice, sample
 * for sample. Every write lands between two samples on both.
 */

#include "Vpsg.h"
#include "Vpsg___024root.h"

#include "psg_shim.h"
#include "utest.h"

extern "C"
{
#include "core/aud/psg.h"
}

#include <cstdio>
#include <cstring>

static Vpsg *dut;

static void clock_cycle()
{
    dut->clk = 0;
    dut->clk_sys = 0;
    dut->eval();
    dut->clk = 1;
    dut->clk_sys = 1;
    dut->eval();
}

static void snoop_dut(uint16_t addr, uint8_t val)
{
    dut->q_we = 1;
    dut->q_addr = addr;
    dut->q_val = val;
    clock_cycle();
    dut->q_we = 0;
}

static void rtl_pointer(uint16_t word)
{
    dut->xaddr_we = 1;
    dut->xaddr_wdata = word;
    clock_cycle();
    dut->xaddr_we = 0;
}

/* The Pocket's aud_psg_map: the pointer, then a wait while psg.sv zeroes
 * the eight voices' registers. The pass is over well before the next sample,
 * so a lockstep run loses none. */
static void rtl_xaddr(uint16_t word)
{
    rtl_pointer(word);
    while (dut->psg_busy)
        clock_cycle();
}

/* Both models strike the gate on the write. psg.sv does it on the clock the
 * write lands, and the shim finds the byte in the block as the machines'
 * mixers do and hands it to psg_put. */
static void xram_write(uint16_t addr, uint8_t val)
{
    shim_xram_write(addr, val);
    snoop_dut(addr, val);
}

static long g_sample;
static void run_lockstep(int *utest_result, int n)
{
    for (int i = 0; i < n; i++)
    {
        int16_t cl, cr;
        psg_sample(&cl, &cr);
        int guard = 0;
        while (!dut->psg_valid && guard++ < 4000)
            clock_cycle();
        ASSERT_LT(guard, 4000);
        if (getenv("PSG_DEBUG")
            && ((int16_t)dut->psg_l != cl || (int16_t)dut->psg_r != cr))
            fprintf(stderr, "psg diff sample=%ld rtl=(%d,%d) c=(%d,%d)\n",
                    g_sample, (int16_t)dut->psg_l, (int16_t)dut->psg_r,
                    cl, cr);
        ASSERT_EQ((int16_t)dut->psg_l, cl);
        ASSERT_EQ((int16_t)dut->psg_r, cr);
        g_sample++;
        clock_cycle();
    }
}

/* A pointer write that lands while psg.sv computes a sample does not
 * reset the eight channels before psg.sv next enters P_IDLE, so that
 * sample finishes on the old state. The caller runs psg_map only after
 * this function returns, so psg.c also finishes that sample on the old
 * state. */
static void rtl_xaddr_mid_walk(int *utest_result, uint16_t word, int depth)
{
    while (dut->rootp->psg__DOT__state == 0)
        clock_cycle();
    for (int i = 0; i < depth; i++)
        clock_cycle();
    rtl_pointer(word);
    run_lockstep(utest_result, 1);
}

/* A byte of the bell's structure into both models, as the sequencer puts
 * it. */
static void bel_put(unsigned i, uint8_t val)
{
    psg_put(PSG_REGS_SIZE + i, val);
    dut->bel_we = 1;
    dut->bel_idx = (uint8_t)i;
    dut->bel_val = val;
    clock_cycle();
    dut->bel_we = 0;
}

/* A strike as bel.c makes it: the gate down, the sound, the gate up. */
static void bel_strike(uint16_t freq, uint8_t duty, uint8_t va, uint8_t vd,
                       uint8_t wr)
{
    bel_put(6, 0);
    bel_put(0, (uint8_t)freq);
    bel_put(1, (uint8_t)(freq >> 8));
    bel_put(2, duty);
    bel_put(3, va);
    bel_put(4, vd);
    bel_put(5, wr);
    bel_put(6, 1);
}

static void bel_silence()
{
    bel_put(6, 0);
    for (unsigned i = 0; i < 6; i++)
        bel_put(i, 0);
}

/* The teletype at 48 kHz: struck, released at 20 ms, restruck at 100 ms
 * from what its release left, and silenced at 800 ms, then a noise bell. */
static void bel_ring(int *utest_result)
{
    bel_strike(1760, 215, 0x51, 0x60, 0x39);
    run_lockstep(utest_result, 960);
    bel_put(6, 0);
    run_lockstep(utest_result, 3840);
    bel_strike(1760, 215, 0x51, 0x60, 0x39);
    run_lockstep(utest_result, 960);
    bel_put(6, 0);
    run_lockstep(utest_result, 33600);
    bel_silence();
    run_lockstep(utest_result, 200);
    bel_strike(4000, 180, 0x30, 0x80, 0x45);
    run_lockstep(utest_result, 2000);
    bel_silence();
    run_lockstep(utest_result, 2000);
}

static void config(uint16_t base, int ch, uint16_t freq, uint8_t duty,
                   uint8_t va, uint8_t vd, uint8_t wr, uint8_t pan_gate)
{
    uint16_t at = base + (uint16_t)(ch * 8);
    xram_write(at + 0, (uint8_t)freq);
    xram_write(at + 1, (uint8_t)(freq >> 8));
    xram_write(at + 2, duty);
    xram_write(at + 3, va);
    xram_write(at + 4, vd);
    xram_write(at + 5, wr);
    xram_write(at + 6, pan_gate);
    xram_write(at + 7, 0);
}

UTEST(psg, lockstep_bit_exact)
{
    run_lockstep(utest_result, 200);
    bel_ring(utest_result);

    const uint16_t base = 0x8000;
    psg_map(base);
    rtl_xaddr(base);
    config(base, 0, 1320, 200, 0x38, 0x9A, 0x05, 0x00);  /* sine */
    config(base, 1, 2600, 128, 0x02, 0x84, 0x17, 0x20);  /* square */
    config(base, 2, 700, 90, 0x11, 0x22, 0x23, 0xE0);    /* saw, left */
    config(base, 3, 431, 255, 0x93, 0x71, 0x39, 0x7E);   /* tri, right */
    config(base, 4, 9999, 180, 0x05, 0x55, 0x44, 0x81);  /* noise */
    config(base, 5, 65535, 30, 0xF0, 0x0F, 0x18, 0x80);  /* square, muted */
    config(base, 6, 1, 255, 0x00, 0xFF, 0x76, 0x02);     /* bad wave 7 */
    config(base, 7, 3000, 64, 0x2A, 0x2B, 0x2C, 0x00);   /* saw */
    run_lockstep(utest_result, 50);

    for (int ch = 0; ch < 8; ch++)
    {
        xram_write((uint16_t)(base + ch * 8 + 6),
                   (uint8_t)(0x01 | (ch == 2 ? 0xE0 : ch == 3 ? 0x7E
                                     : ch == 4 ? 0x81 : ch == 5 ? 0x80
                                                                : 0x00)));
        run_lockstep(utest_result, 400);
    }

    run_lockstep(utest_result, 3000);

    for (int ch = 0; ch < 8; ch++)
        xram_write((uint16_t)(base + ch * 8 + 6), 0x00);
    run_lockstep(utest_result, 2000);

    const uint16_t base2 = 0x7002;
    psg_map(base2);
    rtl_xaddr(base2);
    config(base2, 0, 880, 255, 0x00, 0x00, 0x00, 0x01);
    config(base2, 1, 440, 128, 0x02, 0x30, 0x11, 0x20);
    config(base2, 2, 660, 90, 0x11, 0x50, 0x22, 0xE0);
    config(base2, 3, 220, 255, 0x03, 0x70, 0x33, 0x7E);
    config(base2, 4, 3300, 180, 0x05, 0x20, 0x44, 0x81);
    config(base2, 5, 110, 30, 0x21, 0x10, 0x05, 0x40);
    config(base2, 6, 550, 200, 0x12, 0x40, 0x16, 0x02);
    config(base2, 7, 1100, 64, 0x01, 0x60, 0x27, 0x30);
    run_lockstep(utest_result, 50);
    xram_write((uint16_t)(base2 + 1 * 8 + 6), 0x21);
    xram_write((uint16_t)(base2 + 5 * 8 + 6), 0x41);
    xram_write((uint16_t)(base2 + 7 * 8 + 6), 0x31);
    run_lockstep(utest_result, 800);

    /* Twenty gate writes between two samples, which both models settle on
     * the last of. */
    for (int i = 0; i < 20; i++)
        xram_write((uint16_t)(base2 + 6), (uint8_t)(i & 1));
    run_lockstep(utest_result, 600);

    xram_write((uint16_t)(base2 + 6), 0x80);
    run_lockstep(utest_result, 200);
    xram_write((uint16_t)(base2 + 6), 0x01);
    run_lockstep(utest_result, 200);

    for (int i = 0; i < 20; i++)
        xram_write((uint16_t)(base2 + 8 + (i % 6)), (uint8_t)(i * 7));
    /* The write to base2 + 0x46 is at the pan_gate offset of a channel past
     * the block's eight, so the shim's filter and psg.sv both ignore it. */
    xram_write((uint16_t)(base2 + 0x46), 0x01);
    xram_write((uint16_t)(base2 + 2 * 8 + 6), 0x01);
    for (int i = 0; i < 20; i++)
        xram_write((uint16_t)(base2 + 24 + (i % 6)), (uint8_t)(i * 5));
    run_lockstep(utest_result, 300);

    for (int ch = 0; ch < 8; ch++)
        for (int off = 0; off < 6; off++)
            xram_write((uint16_t)(base2 + ch * 8 + off),
                       (uint8_t)(0x11 * (ch + 1) + off));
    run_lockstep(utest_result, 300);

    xram_write((uint16_t)((base2 & 0xFF00) - 0x100 + 6), 0x01);
    xram_write((uint16_t)((base2 & 0xFF00) + 0x100 + 6), 0x00);
    run_lockstep(utest_result, 100);
    xram_write((uint16_t)(base2 + 6), 0x00);
    run_lockstep(utest_result, 200);

    psg_map(base);
    rtl_xaddr(base);
    config(base, 0, 900, 0, 0x04, 0x00, 0x06, 0x10);
    config(base, 1, 1800, 255, 0x06, 0x00, 0x18, 0xF0);
    config(base, 2, 2700, 0, 0x07, 0x00, 0x2D, 0x00);
    config(base, 3, 3600, 255, 0x09, 0x00, 0x3E, 0x20);
    config(base, 4, 5400, 0, 0x0B, 0x00, 0x4F, 0xE0);
    config(base, 5, 7200, 255, 0x0C, 0x00, 0x06, 0x00);
    config(base, 6, 250, 128, 0x0D, 0x00, 0x18, 0x40);
    config(base, 7, 125, 64, 0x0E, 0x00, 0x2D, 0xC0);
    static const uint8_t pans_a[8] =
        {0x10, 0xF0, 0x00, 0x20, 0xE0, 0x00, 0x40, 0xC0};
    for (int ch = 0; ch < 8; ch++)
        xram_write((uint16_t)(base + ch * 8 + 6),
                   (uint8_t)(pans_a[ch] | 0x01));
    run_lockstep(utest_result, 500);

    psg_map(base);
    rtl_xaddr(base);
    config(base, 0, 900, 200, 0x00, 0x46, 0x00, 0x10);
    config(base, 1, 1800, 128, 0x00, 0x68, 0x10, 0xF0);
    config(base, 2, 2700, 200, 0x00, 0xAD, 0x20, 0x00);
    config(base, 3, 3600, 255, 0x00, 0xBE, 0x30, 0x20);
    config(base, 4, 5400, 180, 0x00, 0xCF, 0x40, 0xE0);
    config(base, 5, 7200, 200, 0x0F, 0xD0, 0x00, 0x00);
    config(base, 6, 250, 128, 0x00, 0xE1, 0x10, 0x40);
    config(base, 7, 125, 200, 0x00, 0xF3, 0x20, 0xC0);
    for (int ch = 0; ch < 8; ch++)
        xram_write((uint16_t)(base + ch * 8 + 6),
                   (uint8_t)(pans_a[ch] | 0x01));
    run_lockstep(utest_result, 3200);
    bel_ring(utest_result);

    /* psg.sv spends 136 clocks on a sample, fifteen a voice and one to
     * output, and each of these depths lands the pointer write inside that
     * span. The eight sound a different chord each time, each voice at a
     * quarter of full volume, so the sample in flight shows a tear. */
    static const int depths[] = {1, 5, 15, 30, 50, 80, 110, 130};
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); i++)
    {
        for (int ch = 0; ch < 8; ch++)
            config(base, ch, (uint16_t)(600 + 170 * ch + 90 * i),
                   (uint8_t)(64 + 24 * ch), 0x90, 0x90,
                   (uint8_t)(((ch + i) % 5) << 4 | 0x02),
                   (uint8_t)(pans_a[ch] | 0x01));
        run_lockstep(utest_result, 30);
        rtl_xaddr_mid_walk(utest_result, base, depths[i]);
        psg_map(base);
        run_lockstep(utest_result, 60);
    }

    psg_map(0xFFFF);
    rtl_xaddr(0xFFFF);
    run_lockstep(utest_result, 10);
    run_lockstep(utest_result, 10);
    rtl_xaddr_mid_walk(utest_result, base, 0);
    psg_map(base);
    run_lockstep(utest_result, 600);

    /* A block may start anywhere: here at an odd address, across a page. */
    const uint16_t base3 = 0x80E1;
    psg_map(base3);
    rtl_xaddr(base3);
    config(base3, 0, 1320, 200, 0x38, 0x9A, 0x05, 0x00);
    config(base3, 1, 2600, 128, 0x02, 0x84, 0x17, 0x20);
    config(base3, 2, 700, 90, 0x11, 0x22, 0x23, 0xE0);
    config(base3, 3, 431, 255, 0x93, 0x71, 0x39, 0x7E);
    config(base3, 4, 9999, 180, 0x05, 0x55, 0x44, 0x81);
    config(base3, 5, 65535, 30, 0xF0, 0x0F, 0x18, 0x80);
    config(base3, 6, 1, 255, 0x00, 0xFF, 0x76, 0x02);
    config(base3, 7, 3000, 64, 0x2A, 0x2B, 0x2C, 0x00);
    for (int ch = 0; ch < 8; ch++)
        xram_write((uint16_t)(base3 + ch * 8 + 6), 0x01);
    run_lockstep(utest_result, 1200);
    for (int ch = 0; ch < 8; ch++)
        xram_write((uint16_t)(base3 + ch * 8 + 6), 0x00);
    run_lockstep(utest_result, 600);

    psg_map(0xFFFF);
    rtl_xaddr(0xFFFF);
}

static void snoop_now(uint16_t addr, uint8_t val)
{
    shim_xram_write(addr, val);
    snoop_dut(addr, val);
}

static int rtl_sample()
{
    int guard = 0;
    while (!dut->psg_valid && guard++ < 4000)
        clock_cycle();
    int l = (int16_t)dut->psg_l;
    clock_cycle();
    return l;
}

static void rtl_reset()
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vpsg;
    dut->clk = 0;
    dut->q_we = 0;
    dut->xaddr_we = 0;
    dut->eval();
    for (int i = 0; i < 4; i++)
        clock_cycle();
}

static void one_loud_channel(uint16_t base)
{
    rtl_xaddr(base);
    config(base, 0, 1000, 255, 0x0F, 0x00, 0x02, 0x00);
}

#define ADSR_RELEASE 0
#define ADSR_ATTACK 1
/* P_VOICE is the second member of state_t in psg.sv, and a voice steps on
 * its clock 10. */
#define PSG_STATE_VOICE 1
#define PSG_T_STEP 10

/* Voice 0's sample is word 0 of psg.sv's voice memory. */
static int16_t rtl_sample0() { return (int16_t)dut->rootp->psg__DOT__vm[0]; }

UTEST(psg, gate_applies_on_the_clock_it_lands)
{
    rtl_reset();
    const uint16_t base = 0x4000;
    one_loud_channel(base);
    rtl_sample();

    ASSERT_EQ(ADSR_RELEASE, dut->rootp->psg__DOT__ch_adsr[0]);
    snoop_now(base + 6, 0x01);
    ASSERT_EQ(ADSR_ATTACK, dut->rootp->psg__DOT__ch_adsr[0]);

    snoop_now(base + 6, 0x00);
    ASSERT_EQ(ADSR_RELEASE, dut->rootp->psg__DOT__ch_adsr[0]);
}

/* Six hundred writes between two samples. Neither model buffers them, so
 * the last one sets the state however many came before it. */
UTEST(psg, no_write_is_dropped)
{
    rtl_reset();
    const uint16_t base = 0x4000;
    one_loud_channel(base);
    rtl_sample();

    for (int i = 0; i < 600; i++)
        snoop_now(base + 6, (uint8_t)(i & 1));
    ASSERT_EQ(ADSR_ATTACK, dut->rootp->psg__DOT__ch_adsr[0]);

    for (int i = 0; i < 601; i++)
        snoop_now(base + 6, (uint8_t)(i & 1));
    ASSERT_EQ(ADSR_RELEASE, dut->rootp->psg__DOT__ch_adsr[0]);
}

/* Wave 1 at duty 255 is 32767 at every phase and wave 5 is 0 at every
 * phase, so the stored sample shows which of the two wave_release values
 * it was computed from. A write on the voice's step comes after the walk
 * read that byte, so the step keeps the old one and the next sample has
 * the new one. */
UTEST(psg, a_write_mid_walk_is_heard_by_the_next_sample)
{
    rtl_reset();
    const uint16_t base = 0x4000;
    rtl_xaddr(base);
    config(base, 0, 1000, 255, 0x0F, 0x00, 0x10, 0x00);
    rtl_sample();
    rtl_sample();
    ASSERT_EQ(32767, rtl_sample0());

    while (dut->rootp->psg__DOT__state != PSG_STATE_VOICE
           || dut->rootp->psg__DOT__ch != 0
           || dut->rootp->psg__DOT__t != PSG_T_STEP)
        clock_cycle();
    snoop_dut(base + 5, 0x50);
    rtl_sample();
    ASSERT_EQ(32767, rtl_sample0());
    rtl_sample();
    ASSERT_EQ(0, rtl_sample0());
}

/* Eight sounding voices, then a pointer write, which releases them and
 * zeroes their registers, and leaves the bell's row. */
UTEST(psg, a_pointer_write_clears_the_registers)
{
    rtl_reset();
    const uint16_t base = 0x4000;
    rtl_xaddr(base);
    for (int ch = 0; ch < 8; ch++)
        config(base, ch, 1000, 255, 0x0F, 0x00, 0x10,
               (uint8_t)(0x01 | (ch << 4)));
    bel_put(0, 0x5A);
    rtl_sample();
    for (int ch = 0; ch < 8; ch++)
        ASSERT_EQ(ADSR_ATTACK, dut->rootp->psg__DOT__ch_adsr[ch]);

    rtl_xaddr(base);
    for (int i = 0; i < 64; i++)
        ASSERT_EQ(0, dut->rootp->psg__DOT__cfg[i]);
    ASSERT_EQ(0x5A, dut->rootp->psg__DOT__cfg[64]);
    for (int ch = 0; ch < 8; ch++)
        ASSERT_EQ(ADSR_RELEASE, dut->rootp->psg__DOT__ch_adsr[ch]);
}

/* The constants are PHASE_MAGIC and PHASE_SHIFT from psg.sv, and 144000
 * is its PHASE_DIV. Every frequency is checked because the constant for a
 * shift of 25, 24 or 23, rounded up, is right for most of them: it first
 * fails at 53932 for a shift of 25 or 24 and at 12307 for a shift of 23. */
UTEST(psg, the_phase_magic_is_a_division)
{
    for (uint32_t f = 0; f < 65536; f++)
    {
        uint32_t magic = (uint32_t)(((uint64_t)f * 2001599834387ULL) >> 26);
        uint32_t divided = (uint32_t)(((uint64_t)f << 32) / 144000ULL);
        ASSERT_EQ(magic, divided);
    }
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vpsg;
    dut->clk = 0;
    dut->eval();
    for (int i = 0; i < 4; i++)
        clock_cycle();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
