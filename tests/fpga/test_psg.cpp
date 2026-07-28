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
 */

#include "Vaud_psg.h"

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

    /* The halfword-aligned block, reprogrammed live. */
    const uint16_t base2 = 0x7002;
    config(base2, 0, 880, 255, 0x00, 0x00, 0x00, 0x01);
    for (int ch = 1; ch < 8; ch++)
        config(base2, ch, 0, 0, 0, 0, 0, 0);
    ASSERT_TRUE(psg_xreg(base2));
    rtl_xaddr(base2);
    run_lockstep(utest_result, 800);

    /* Flood the queue past a sample's 32-entry drain and past the
     * ring itself: order preserved, drops matched. */
    for (int i = 0; i < 300; i++)
        xram_write((uint16_t)(base2 + 6), (uint8_t)(i & 1));
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
