/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The PSG in lockstep: the vendored psg.c drives its handler sample by
 * sample over shim seams while the verilated aud_psg runs the same
 * configs from the same XRAM image, gates injected at identical sample
 * indices on both sides — every stereo PWM pair must match exactly.
 * Covers all five waves across the channels, the envelope through
 * attack, decay, the sustain quirks, and release, pan extremes with
 * the truncating division and the mute, the halfword-aligned block,
 * and the gate queue's 32-per-sample drain and drop-when-full ring.
 * The bell rings on both sides too — the real bel.c against aud_bel —
 * first standing alone with the pointer parked, then under the song.
 * The hostile phases press where the plumbing bends: device-register
 * writes landing deep inside a walk, non-gate and wrong-page traffic
 * through the queue filters, the mute over a sounding channel, the
 * halfword block loaded to its seventeenth word, and an envelope sweep
 * that pins every table constant the song phases left dark.
 */

#include "Vaud_psg.h"
#include "Vaud_psg___024root.h"

#include "psg_shim.h"
#include "utest.h"

extern "C"
{
#include "ria/aud/bel.h"
}

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" bool psg_xreg(uint16_t word);

static Vaud_psg *dut;

/* Serve the XRAM read channel the machine rotor's way: grant on the
 * spot, the word one clock later, both sides reading the shim's XRAM. */
static uint16_t gnt_addr;
static bool gnt_pending;

static void clock_cycle()
{
    dut->clk = 0;
    dut->eval();
    dut->a_gnt = dut->aud_psg_a_req;
    if (gnt_pending)
        dut->a_rdata = shim_xram_word(gnt_addr);
    gnt_pending = dut->a_gnt;
    if (dut->a_gnt)
        gnt_addr = dut->aud_psg_a_addr;
    dut->eval();
    dut->clk = 1;
    dut->eval();
}

static void rtl_xaddr(uint16_t word)
{
    dut->xaddr_we = 1;
    dut->xaddr_wdata = word;
    clock_cycle();
    dut->xaddr_we = 0;
    clock_cycle(); /* the write applies at the boundary, one clock on */
}

/* One XRAM byte written by the "6502": both models see the data and
 * their gate queues see the notify. */
static void xram_write(uint16_t addr, uint8_t val)
{
    shim_xram_write(addr, val);
    dut->q_we = 1;
    dut->q_addr = addr;
    dut->q_val = val;
    clock_cycle();
    dut->q_we = 0;
}

/* Run both models for n samples, demanding exact agreement. */
static long g_sample;
static void run_lockstep(int *utest_result, int n)
{
    for (int i = 0; i < n; i++)
    {
        uint16_t cl, cr;
        shim_sample(&cl, &cr);
        int guard = 0;
        while (!dut->aud_psg_valid && guard++ < 4000)
            clock_cycle();
        ASSERT_LT(guard, 4000);
        if (getenv("PSG_DEBUG")
            && (dut->aud_psg_l != cl || dut->aud_psg_r != cr))
            fprintf(stderr, "psg diff sample=%ld rtl=(%d,%d) c=(%d,%d)\n",
                    g_sample, dut->aud_psg_l, dut->aud_psg_r, cl, cr);
        ASSERT_EQ(dut->aud_psg_l, cl);
        ASSERT_EQ(dut->aud_psg_r, cr);
        g_sample++;
        clock_cycle(); /* consume the strobe */
    }
}

/* Land a device-register write depth clocks into an in-flight walk —
 * where a torn reset would shear the sample — then finish that sample
 * on the old state, the ISR's own ordering. The caller runs psg_xreg
 * on the C side after this returns, completing the boundary. */
static void rtl_xaddr_mid_walk(int *utest_result, uint16_t word, int depth)
{
    while (dut->rootp->aud_psg__DOT__state == 0)
        clock_cycle();
    for (int i = 0; i < depth; i++)
        clock_cycle();
    dut->xaddr_we = 1;
    dut->xaddr_wdata = word;
    clock_cycle();
    dut->xaddr_we = 0;
    run_lockstep(utest_result, 1);
}

/* One teletype bell on both machines. */
static void bel_ring()
{
    bel_add(&bel_teletype);
    dut->bel_strike = 1;
    clock_cycle();
    dut->bel_strike = 0;
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
    bel_setup();

    /* The standing bell, the machine as aud_init leaves it: parked
     * pointer, silence, then one teletype bell through attack, decay,
     * sustain, the 20 ms release, out to the 800 ms end. */
    run_lockstep(utest_result, 30);
    bel_ring();
    run_lockstep(utest_result, 19300);

    /* Three queued bells restrike at 100 ms strides; ten more flood
     * the ring of eight, the drops matched, the last rings out. */
    bel_ring();
    bel_ring();
    bel_ring();
    run_lockstep(utest_result, 5000);
    for (int i = 0; i < 10; i++)
        bel_ring();
    run_lockstep(utest_result, 36000);

    /* Every wave across the channels at a word-aligned block. */
    const uint16_t base = 0x8000;
    config(base, 0, 1320, 200, 0x38, 0x9A, 0x05, 0x00);  /* sine */
    config(base, 1, 2600, 128, 0x02, 0x84, 0x17, 0x20);  /* square */
    config(base, 2, 700, 90, 0x11, 0x22, 0x23, 0xE0);    /* saw, left */
    config(base, 3, 431, 255, 0x93, 0x71, 0x39, 0x7E);   /* tri, right */
    config(base, 4, 9999, 180, 0x05, 0x55, 0x44, 0x81);  /* noise */
    config(base, 5, 65535, 30, 0xF0, 0x0F, 0x18, 0x80);  /* sine, muted */
    config(base, 6, 1, 255, 0x00, 0xFF, 0x76, 0x02);     /* bad wave 7 */
    config(base, 7, 3000, 64, 0x2A, 0x2B, 0x2C, 0x00);   /* square-ish */

    ASSERT_TRUE(psg_xreg(base));
    rtl_xaddr(base);
    run_lockstep(utest_result, 50);

    /* Gates up one by one — attack through decay into sustain. */
    for (int ch = 0; ch < 8; ch++)
    {
        xram_write((uint16_t)(base + ch * 8 + 6),
                   (uint8_t)(0x01 | (ch == 2 ? 0xE0 : ch == 3 ? 0x7E
                                     : ch == 4 ? 0x81 : ch == 5 ? 0x80
                                                                : 0x00)));
        run_lockstep(utest_result, 400);
    }

    /* A bell under the full mix: mixed after the channel shift, it
     * rides the handler switch and the reprogram below unbroken. */
    bel_ring();
    run_lockstep(utest_result, 3000);

    /* Gates down: release to the floor. */
    for (int ch = 0; ch < 8; ch++)
        xram_write((uint16_t)(base + ch * 8 + 6), 0x00);
    run_lockstep(utest_result, 2000);

    /* The halfword-aligned block, reprogrammed live and loaded to its
     * seventeenth word: channel 7's pan rides the last fetched bytes,
     * and gates land across the borrowing offsets. */
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

    /* Flood the queue past a sample's 32-entry drain and past the
     * ring itself: order preserved, drops matched. */
    for (int i = 0; i < 300; i++)
        xram_write((uint16_t)(base2 + 6), (uint8_t)(i & 1));
    run_lockstep(utest_result, 600);

    /* The mute over a sounding channel: the flood left channel 0
     * attacking at full volume; pan -64 with the gate edge must cut
     * the mix while the release still carries real volume. */
    xram_write((uint16_t)(base2 + 6), 0x80);
    run_lockstep(utest_result, 200);
    xram_write((uint16_t)(base2 + 6), 0x01);
    run_lockstep(utest_result, 200);

    /* Non-gate traffic through the live page: config bytes stream
     * through the queue like the tracker's, burning drain budget but
     * never gating; offset 70 passes the stride and fails the channel
     * bound; a real gate rides the middle of the burst. */
    for (int i = 0; i < 20; i++)
        xram_write((uint16_t)(base2 + 8 + (i % 6)), (uint8_t)(i * 7));
    xram_write((uint16_t)(base2 + 0x46), 0x01);
    xram_write((uint16_t)(base2 + 2 * 8 + 6), 0x01);
    for (int i = 0; i < 20; i++)
        xram_write((uint16_t)(base2 + 24 + (i % 6)), (uint8_t)(i * 5));
    run_lockstep(utest_result, 300);

    /* Near-miss pages: gates one page off either side must not
     * enqueue; the real page still lands afterward. */
    xram_write((uint16_t)((base2 & 0xFF00) - 0x100 + 6), 0x01);
    xram_write((uint16_t)((base2 & 0xFF00) + 0x100 + 6), 0x00);
    run_lockstep(utest_result, 100);
    xram_write((uint16_t)(base2 + 6), 0x00);
    run_lockstep(utest_result, 200);

    /* The attack-rate sweep: the nibbles the song phases never used,
     * ramping toward full volume — a wrong table constant diverges
     * within a sample. Duty boundaries 0 and 255 ride along. */
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

    /* The sustain-target and decay sweep: fast attacks to the top,
     * then every unvisited volume level and decay rate, channel 5
     * carrying the 8-second attack nibble. */
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

    /* Device-register writes landing inside the walk: shallow in the
     * fetch, in the mix, deep in the generate, and into the drain with
     * gates pending — the reset must hold whole at the boundary. */
    static const int depths[] = {3, 40, 60, 200, 400, 424};
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); i++)
    {
        for (int ch = 0; ch < 8; ch++)
            xram_write((uint16_t)(base + ch * 8 + 6),
                       (uint8_t)((i & 1) ? 0x00 : 0x01));
        rtl_xaddr_mid_walk(utest_result, base, depths[i]);
        ASSERT_TRUE(psg_xreg(base));
        run_lockstep(utest_result, 60);
    }

    /* And into a bell-only walk: park the pointer, ring, then land the
     * enable inside the short standing walk. */
    ASSERT_TRUE(psg_xreg(0xFFFF));
    rtl_xaddr(0xFFFF);
    run_lockstep(utest_result, 10);
    bel_ring();
    run_lockstep(utest_result, 10);
    rtl_xaddr_mid_walk(utest_result, base, 0);
    ASSERT_TRUE(psg_xreg(base));
    run_lockstep(utest_result, 600);

    /* Reject parity: the pointer that fails leaves both silent. */
    ASSERT_FALSE(psg_xreg(0x80F2)); /* block crosses its page */
    rtl_xaddr(0xFFFF);
    ASSERT_TRUE(psg_xreg(0xFFFF));
    rtl_xaddr(0xFFFF);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vaud_psg;
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
        clock_cycle();
    dut->rst_n = 1;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
