/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * psg.sv mixes the bell into its output as a ninth voice and psg.c does
 * not, so the two agree only while the bell is silent. Nothing in this
 * file writes the bell's registers.
 */

#include "Vpsg.h"
#include "Vpsg___024root.h"

#include "psg_shim.h"
#include "utest.h"

extern "C"
{
#include "core/aud/bel.h"
#include "core/aud/psg.h"
}

#include <cstdio>
#include <cstring>

extern "C" bool psg_xreg(uint16_t word);

static Vpsg *dut;

static void clock_cycle()
{
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
}

static void snoop_dut(uint16_t addr, uint8_t val, bool host)
{
    dut->q_we = 1;
    dut->q_host = host;
    dut->q_addr = addr;
    dut->q_val = val;
    clock_cycle();
    dut->q_we = 0;
    dut->q_host = 1;
}

/* The Pocket's psg_xreg loads the block into psg.sv by writing each byte
 * over itself, because psg.sv takes the channel registers only from the
 * XRAM writes it snoops. Those writes come from the soft CPU with q_host
 * low, so they strike no gate. The shim's XRAM is not written, because a
 * write there would strike gates on psg.c that never happen on the
 * RP2350. */
static void rtl_import(uint16_t base)
{
    for (uint16_t i = 0; i < 64; i++)
        snoop_dut((uint16_t)(base + i), shim_xram_read((uint16_t)(base + i)),
                  false);
}

static void rtl_xaddr(uint16_t word)
{
    dut->xaddr_we = 1;
    dut->xaddr_wdata = word;
    clock_cycle();
    dut->xaddr_we = 0;
    clock_cycle();
    if (word != 0xFFFF)
        rtl_import(word);
}

/* Both models strike the gate on the write. psg.sv does it on the clock the
 * write lands, and the shim hands the byte straight to psg_xram_write, which
 * is what the RW engine does on every machine but the Pico. */
static void xram_write(uint16_t addr, uint8_t val)
{
    shim_xram_write(addr, val);
    snoop_dut(addr, val, true);
}

static long g_sample;
static void run_lockstep(int *utest_result, int n)
{
    for (int i = 0; i < n; i++)
    {
        int16_t cl, cr;
        shim_sample(&cl, &cr);
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
 * sample finishes on the old state. The caller runs psg_xreg only after
 * this function returns, so psg.c also finishes that sample on the old
 * state. */
static void rtl_xaddr_mid_walk(int *utest_result, uint16_t word, int depth)
{
    while (dut->rootp->psg__DOT__state == 0)
        clock_cycle();
    for (int i = 0; i < depth; i++)
        clock_cycle();
    dut->xaddr_we = 1;
    dut->xaddr_wdata = word;
    clock_cycle();
    dut->xaddr_we = 0;
    /* The sample in progress is compared before the import, because the
     * import takes 64 clocks and psg.sv spends 55 on a sample. psg_valid is
     * high for one clock, so importing first would let this sample's pulse
     * end before run_lockstep polls for it, and run_lockstep would compare
     * psg.c's sample with psg.sv's next one. */
    run_lockstep(utest_result, 1);
    if (word != 0xFFFF)
        rtl_import(word);
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
    shim_init();
    bel_init();

    run_lockstep(utest_result, 200);

    const uint16_t base = 0x8000;
    config(base, 0, 1320, 200, 0x38, 0x9A, 0x05, 0x00);  /* sine */
    config(base, 1, 2600, 128, 0x02, 0x84, 0x17, 0x20);  /* square */
    config(base, 2, 700, 90, 0x11, 0x22, 0x23, 0xE0);    /* saw, left */
    config(base, 3, 431, 255, 0x93, 0x71, 0x39, 0x7E);   /* tri, right */
    config(base, 4, 9999, 180, 0x05, 0x55, 0x44, 0x81);  /* noise */
    config(base, 5, 65535, 30, 0xF0, 0x0F, 0x18, 0x80);  /* square, muted */
    config(base, 6, 1, 255, 0x00, 0xFF, 0x76, 0x02);     /* bad wave 7 */
    config(base, 7, 3000, 64, 0x2A, 0x2B, 0x2C, 0x00);   /* saw */

    ASSERT_TRUE(psg_xreg(base));
    rtl_xaddr(base);
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
    config(base2, 0, 880, 255, 0x00, 0x00, 0x00, 0x01);
    config(base2, 1, 440, 128, 0x02, 0x30, 0x11, 0x20);
    config(base2, 2, 660, 90, 0x11, 0x50, 0x22, 0xE0);
    config(base2, 3, 220, 255, 0x03, 0x70, 0x33, 0x7E);
    config(base2, 4, 3300, 180, 0x05, 0x20, 0x44, 0x81);
    config(base2, 5, 110, 30, 0x21, 0x10, 0x05, 0x40);
    config(base2, 6, 550, 200, 0x12, 0x40, 0x16, 0x02);
    config(base2, 7, 1100, 64, 0x01, 0x60, 0x27, 0x30);
    ASSERT_TRUE(psg_xreg(base2));
    rtl_xaddr(base2);
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
     * the block's eight, so psg.c and psg.sv both ignore it. */
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
    ASSERT_TRUE(psg_xreg(base));
    rtl_xaddr(base);
    for (int ch = 0; ch < 8; ch++)
        xram_write((uint16_t)(base + ch * 8 + 6),
                   (uint8_t)(pans_a[ch] | 0x01));
    run_lockstep(utest_result, 500);

    config(base, 0, 900, 200, 0x00, 0x46, 0x00, 0x10);
    config(base, 1, 1800, 128, 0x00, 0x68, 0x10, 0xF0);
    config(base, 2, 2700, 200, 0x00, 0xAD, 0x20, 0x00);
    config(base, 3, 3600, 255, 0x00, 0xBE, 0x30, 0x20);
    config(base, 4, 5400, 180, 0x00, 0xCF, 0x40, 0xE0);
    config(base, 5, 7200, 200, 0x0F, 0xD0, 0x00, 0x00);
    config(base, 6, 250, 128, 0x00, 0xE1, 0x10, 0x40);
    config(base, 7, 125, 200, 0x00, 0xF3, 0x20, 0xC0);
    ASSERT_TRUE(psg_xreg(base));
    rtl_xaddr(base);
    for (int ch = 0; ch < 8; ch++)
        xram_write((uint16_t)(base + ch * 8 + 6),
                   (uint8_t)(pans_a[ch] | 0x01));
    run_lockstep(utest_result, 3200);

    /* psg.sv spends 55 clocks on a sample, from entering P_MIX to leaving
     * P_STEP, and each of these depths lands the pointer write inside that
     * span. */
    static const int depths[] = {1, 5, 15, 30, 50};
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); i++)
    {
        for (int ch = 0; ch < 8; ch++)
            xram_write((uint16_t)(base + ch * 8 + 6),
                       (uint8_t)((i & 1) ? 0x00 : 0x01));
        rtl_xaddr_mid_walk(utest_result, base, depths[i]);
        ASSERT_TRUE(psg_xreg(base));
        run_lockstep(utest_result, 60);
    }

    ASSERT_TRUE(psg_xreg(0xFFFF));
    rtl_xaddr(0xFFFF);
    run_lockstep(utest_result, 10);
    run_lockstep(utest_result, 10);
    rtl_xaddr_mid_walk(utest_result, base, 0);
    ASSERT_TRUE(psg_xreg(base));
    run_lockstep(utest_result, 600);

    ASSERT_FALSE(psg_xreg(0x80F2)); /* the block crosses into page 0x81 */
    rtl_xaddr(0xFFFF);
    ASSERT_TRUE(psg_xreg(0xFFFF));
    rtl_xaddr(0xFFFF);
}

static void snoop_now(uint16_t addr, uint8_t val)
{
    shim_xram_write(addr, val);
    snoop_dut(addr, val, true);
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
    dut->q_host = 1;
    dut->xaddr_we = 0;
    dut->eval();
    for (int i = 0; i < 4; i++)
        clock_cycle();
}

static void one_loud_channel(uint16_t base)
{
    for (int ch = 1; ch < 8; ch++)
        config(base, ch, 0, 0, 0, 0, 0, 0);
    config(base, 0, 1000, 255, 0x0F, 0x00, 0x02, 0x00);
    rtl_xaddr(base);
}

#define ADSR_RELEASE 0
#define ADSR_ATTACK 1
/* P_STEP is the sixth member of state_t in psg.sv, so its value is 5. */
#define PSG_STATE_STEP 5

UTEST(psg, gate_applies_on_the_clock_it_lands)
{
    shim_init();
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
    shim_init();
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
 * phase, so the sample stored in P_STEP shows which of the two
 * wave_release values it was computed from. */
UTEST(psg, a_write_reaches_the_step_it_lands_on)
{
    shim_init();
    rtl_reset();
    const uint16_t base = 0x4000;
    for (int ch = 1; ch < 8; ch++)
        config(base, ch, 0, 0, 0, 0, 0, 0);
    config(base, 0, 1000, 255, 0x0F, 0x00, 0x10, 0x00);
    rtl_xaddr(base);
    rtl_sample();
    rtl_sample();
    ASSERT_EQ(32767, (int16_t)dut->rootp->psg__DOT__ch_sample[0]);

    while (dut->rootp->psg__DOT__state != PSG_STATE_STEP
           || dut->rootp->psg__DOT__ch != 0)
        clock_cycle();
    snoop_dut(base + 5, 0x50, false);
    ASSERT_EQ(0, (int16_t)dut->rootp->psg__DOT__ch_sample[0]);
}

UTEST(psg, an_imported_block_carries_no_gate)
{
    shim_init();
    rtl_reset();
    const uint16_t base = 0x4000;
    for (int ch = 0; ch < 8; ch++)
        config(base, ch, 1000, 255, 0x0F, 0x00, 0x10,
               (uint8_t)(0x01 | (ch << 4)));
    rtl_xaddr(base);
    rtl_sample();
    rtl_sample();
    for (int ch = 0; ch < 8; ch++)
        ASSERT_EQ(ADSR_RELEASE, dut->rootp->psg__DOT__ch_adsr[ch]);

    snoop_now(base + 6, 0x01);
    ASSERT_EQ(ADSR_ATTACK, dut->rootp->psg__DOT__ch_adsr[0]);
    ASSERT_EQ(32767, (int16_t)dut->rootp->psg__DOT__ch_sample[0]);
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
    dut->q_host = 1;
    dut->eval();
    for (int i = 0; i < 4; i++)
        clock_cycle();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
