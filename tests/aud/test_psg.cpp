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
 * The bell is not here. It belongs to the machine now rather than to
 * this engine — one instance past the engine mux in rp6502.sv — and
 * test_aud rings it end to end.
 *
 * So do not ring one in this file. psg.c still mixes the bell, because
 * on the RP2350 and in the emulator the driver is the whole output
 * stage and there is nowhere else for it to go; aud_psg does not,
 * because on the Pocket there is somewhere else. The two agree exactly
 * while the bell is silent, which is the only condition this lockstep
 * is valid under.
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
#include "core/aud/bel.h"
#include "core/aud/psg.h"
}

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

extern "C" bool psg_xreg(uint16_t word);

static Vaud_psg *dut;

static void clock_cycle()
{
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
}

/* Gate edges waiting for the end of the RTL's walk; see xram_write. */
static std::vector<std::pair<uint16_t, uint8_t>> held;

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

/* aud_psg_xreg's import, which is how a block programmed before the
 * pointer reaches an engine that only hears writes. The soft CPU reads
 * each byte and writes it back over itself; the firmware needs nothing
 * because it reads the block every sample. q_host low, so the pan_gate
 * bytes carry their gate bit without striking it — the firmware's ring
 * is discarded at the same moment and neither machine sounds.
 *
 * The shim's XRAM is not written: these bytes are already in it, and a
 * write there would queue snoops psg.c never sees on the RP2350. */
static void rtl_import(uint16_t base)
{
    for (uint16_t i = 0; i < 64; i++)
        snoop_dut((uint16_t)(base + i), shim_xram_read((uint16_t)(base + i)),
                  false);
}

static void rtl_xaddr(uint16_t word)
{
    held.clear();
    dut->xaddr_we = 1;
    dut->xaddr_wdata = word;
    clock_cycle();
    dut->xaddr_we = 0;
    clock_cycle(); /* the reset applies at the boundary, one clock on */
    if (word != 0xFFFF)
        rtl_import(word);
}

/* One XRAM byte written by the "6502", in the two halves the firmware
 * takes it in. The byte itself is read live there, so it reaches this
 * engine's registers now, where a re-read would have found it — the
 * import's own path, which carries a byte and strikes nothing.
 *
 * The gate is the half that has to wait. The firmware replays its ring
 * after the envelope step, so a gate written now reaches the step one
 * sample from now; the RTL acts on the clock the write lands and would
 * reach this sample's step. Both are one step of a 24 kHz envelope and
 * neither is more right, but the waveform only compares if the two
 * models step the same envelope with the same gate. So the edge is held
 * to the end of the RTL's walk, where the firmware's replay sits, and a
 * pointer change discards what is held, because psg_xreg discards the
 * ring (psg.c:284). Where a gate lands is asserted on its own, below. */
static void release_snoop()
{
    for (auto &w : held)
        snoop_dut(w.first, w.second, true);
    held.clear();
}

static void xram_write(uint16_t addr, uint8_t val)
{
    shim_xram_write(addr, val);
    snoop_dut(addr, val, false);
    held.emplace_back(addr, val);
}

/* Run both models for n samples, demanding exact agreement. */
static long g_sample;
static void run_lockstep(int *utest_result, int n)
{
    for (int i = 0; i < n; i++)
    {
        int16_t cl, cr;
        shim_sample(&cl, &cr);
        int guard = 0;
        while (!dut->aud_psg_valid && guard++ < 4000)
            clock_cycle();
        ASSERT_LT(guard, 4000);
        if (getenv("PSG_DEBUG")
            && ((int16_t)dut->aud_psg_l != cl || (int16_t)dut->aud_psg_r != cr))
            fprintf(stderr, "psg diff sample=%ld rtl=(%d,%d) c=(%d,%d)\n",
                    g_sample, (int16_t)dut->aud_psg_l, (int16_t)dut->aud_psg_r,
                    cl, cr);
        ASSERT_EQ((int16_t)dut->aud_psg_l, cl);
        ASSERT_EQ((int16_t)dut->aud_psg_r, cr);
        g_sample++;
        clock_cycle(); /* consume the strobe */
        release_snoop();
    }
}

/* Land a device-register write depth clocks into an in-flight walk —
 * where a torn reset would shear the sample — then finish that sample
 * on the old state, the ISR's own ordering. The caller runs psg_xreg
 * on the C side after this returns, completing the boundary. */
static void rtl_xaddr_mid_walk(int *utest_result, uint16_t word, int depth)
{
    held.clear(); /* psg_xreg drops the ring; the pointer paths must agree */
    while (dut->rootp->aud_psg__DOT__state == 0)
        clock_cycle();
    for (int i = 0; i < depth; i++)
        clock_cycle();
    dut->xaddr_we = 1;
    dut->xaddr_wdata = word;
    clock_cycle();
    dut->xaddr_we = 0;
    /* The sample the write landed in is finished and compared before the
     * import runs. The import is sixty-four clocks and the walk is barely
     * more than that, so importing here would spend the strobe this
     * sample ends on and pair the firmware's answer against the next one. */
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
    psg_setup(PSG_SHIM_RATE);
    bel_setup();

    /* The machine as aud_init leaves it: pointer parked, and the walk
     * still running so the output stage keeps its sample tick. The bell
     * used to ring here and this test swept it up for free; there is one
     * bell for the machine now, past the engine mux, and test_bel holds
     * it against bel.c on its own. */
    run_lockstep(utest_result, 200);

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
    run_lockstep(utest_result, 3000);

    /* Gates down: release to the floor. */
    for (int ch = 0; ch < 8; ch++)
        xram_write((uint16_t)(base + ch * 8 + 6), 0x00);
    run_lockstep(utest_result, 2000);

    /* The halfword-aligned block, reprogrammed live: every offset in the
     * window decodes off a base that is not word aligned, and gates land
     * across the far channels. */
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

    /* A burst of gates on one channel, inside what the firmware's ring
     * can hold and inside one sample's drain, so both models see every
     * one of them. Past those bounds the firmware starts dropping and
     * rate limiting and the RTL does not; that is asserted by itself in
     * psg.no_write_is_dropped rather than compared here. */
    for (int i = 0; i < 20; i++)
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

    /* Every field of every channel, live, under a base that is not word
     * aligned: an offset has to place a byte from an unaligned base as
     * exactly as from an aligned one. */
    for (int ch = 0; ch < 8; ch++)
        for (int off = 0; off < 6; off++)
            xram_write((uint16_t)(base2 + ch * 8 + off),
                       (uint8_t)(0x11 * (ch + 1) + off));
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

    /* Device-register writes landing inside the walk: across the mix, at
     * its seam, and through the generate — the pointer moves under them,
     * and the reset it carries must still hold whole at the boundary.
     *
     * The walk is fifty-five clocks now that the phase is a multiply
     * rather than nine divisions, so these are the depths that land in
     * it. Past its end the two machines part company for reasons the
     * oracle cannot arbitrate — the fabric applies the reset in the idle
     * it is already standing in, the firmware at its next handler — and
     * the sample that straddles the difference is nobody's bug. */
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

    /* And into a bell-only walk: park the pointer, ring, then land the
     * enable inside the short standing walk. */
    ASSERT_TRUE(psg_xreg(0xFFFF));
    rtl_xaddr(0xFFFF);
    run_lockstep(utest_result, 10);
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

/* Straight to the snoop, nothing held: what the 6502 actually does. */
static void snoop_now(uint16_t addr, uint8_t val)
{
    shim_xram_write(addr, val);
    snoop_dut(addr, val, true);
}

static int rtl_sample()
{
    int guard = 0;
    while (!dut->aud_psg_valid && guard++ < 4000)
        clock_cycle();
    int l = (int16_t)dut->aud_psg_l;
    clock_cycle();
    return l;
}

/* The engine takes no reset, so a clean one is a fresh one. */
static void rtl_reset()
{
    held.clear();
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vaud_psg;
    dut->clk = 0;
    dut->q_we = 0;
    dut->q_host = 1;
    dut->xaddr_we = 0;
    dut->eval();
    for (int i = 0; i < 4; i++)
        clock_cycle();
}

/* One channel at the fastest attack, silent until gated. */
static void one_loud_channel(uint16_t base)
{
    for (int ch = 1; ch < 8; ch++)
        config(base, ch, 0, 0, 0, 0, 0, 0);
    config(base, 0, 1000, 255, 0x0F, 0x00, 0x02, 0x00);
    rtl_xaddr(base);
}

/* The contract that removing the ring creates, and the one thing the
 * lockstep holds its snoops back to avoid: a write reaches the state it
 * names on the clock it lands. The firmware replays after its envelope
 * step and cannot, so there is nothing to compare against and the
 * machine's own behaviour is stated instead. Envelope state, not sound
 * — how long a gate takes to become audible is a question about attack
 * rates, and this is a question about when the write arrives. */
#define ADSR_RELEASE 0
#define ADSR_ATTACK 1
/* P_STEP, the last member of state_t in aud_psg.sv. Sequential, not the
 * one-hot Quartus re-encodes to; move it when the walk gains or loses a
 * state. */
#define PSG_STATE_STEP 5

UTEST(psg, gate_applies_on_the_clock_it_lands)
{
    shim_init();
    psg_setup(PSG_SHIM_RATE);
    rtl_reset();
    const uint16_t base = 0x4000;
    one_loud_channel(base);
    rtl_sample();

    ASSERT_EQ(ADSR_RELEASE, dut->rootp->aud_psg__DOT__ch_adsr[0]);
    snoop_now(base + 6, 0x01);
    ASSERT_EQ(ADSR_ATTACK, dut->rootp->aud_psg__DOT__ch_adsr[0]);

    snoop_now(base + 6, 0x00);
    ASSERT_EQ(ADSR_RELEASE, dut->rootp->aud_psg__DOT__ch_adsr[0]);
}

/* And no bound on how many land. The firmware keeps 256 of them and
 * replays at most 32 a sample, so a burst past either is dropped or
 * deferred; nothing here holds a write long enough to lose one. The
 * last of them wins because it is last, not because it fitted. */
UTEST(psg, no_write_is_dropped)
{
    shim_init();
    psg_setup(PSG_SHIM_RATE);
    rtl_reset();
    const uint16_t base = 0x4000;
    one_loud_channel(base);
    rtl_sample();

    /* Far past the ring, and past any one sample's replay. */
    for (int i = 0; i < 600; i++)
        snoop_now(base + 6, (uint8_t)(i & 1));
    ASSERT_EQ(ADSR_ATTACK, dut->rootp->aud_psg__DOT__ch_adsr[0]);

    for (int i = 0; i < 601; i++)
        snoop_now(base + 6, (uint8_t)(i & 1));
    ASSERT_EQ(ADSR_RELEASE, dut->rootp->aud_psg__DOT__ch_adsr[0]);
}

/* The engine holds its registers in memory the walk reads as it goes,
 * so a write to the voice the cursor is standing on has to reach that
 * clock's own read — the same contract as the gate, and unforwarded it
 * is whatever the silicon does with a read of an array being written.
 * Wave 1 at duty 255 rails high whatever the phase is and wave 5 is
 * silent whatever the phase is, so what the step stored says which of
 * the two bytes it read. */
UTEST(psg, a_write_reaches_the_step_it_lands_on)
{
    shim_init();
    psg_setup(PSG_SHIM_RATE);
    rtl_reset();
    const uint16_t base = 0x4000;
    for (int ch = 1; ch < 8; ch++)
        config(base, ch, 0, 0, 0, 0, 0, 0);
    config(base, 0, 1000, 255, 0x0F, 0x00, 0x10, 0x00);
    rtl_xaddr(base);
    /* The import lands inside whatever walk was in flight, so the block
     * is whole from the next one. */
    rtl_sample();
    rtl_sample();
    ASSERT_EQ(32767, (int16_t)dut->rootp->aud_psg__DOT__ch_sample[0]);

    while (dut->rootp->aud_psg__DOT__state != PSG_STATE_STEP
           || dut->rootp->aud_psg__DOT__ch != 0)
        clock_cycle();
    snoop_dut(base + 5, 0x50, false);
    ASSERT_EQ(0, (int16_t)dut->rootp->aud_psg__DOT__ch_sample[0]);
}

/* A block programmed before its pointer. Every byte of it arrives, by
 * the import the soft CPU runs, and not one of its gates strikes: the
 * firmware reads the same bytes live and throws its ring away, so
 * neither machine sounds until a gate is written again. */
UTEST(psg, an_imported_block_carries_no_gate)
{
    shim_init();
    psg_setup(PSG_SHIM_RATE);
    rtl_reset();
    const uint16_t base = 0x4000;
    for (int ch = 0; ch < 8; ch++)
        config(base, ch, 1000, 255, 0x0F, 0x00, 0x10,
               (uint8_t)(0x01 | (ch << 4)));
    rtl_xaddr(base);
    rtl_sample();
    rtl_sample();
    for (int ch = 0; ch < 8; ch++)
        ASSERT_EQ(ADSR_RELEASE, dut->rootp->aud_psg__DOT__ch_adsr[ch]);

    /* And they did arrive: a gate written now finds the imported wave. */
    snoop_now(base + 6, 0x01);
    ASSERT_EQ(ADSR_ATTACK, dut->rootp->aud_psg__DOT__ch_adsr[0]);
    ASSERT_EQ(32767, (int16_t)dut->rootp->aud_psg__DOT__ch_sample[0]);
}

/* The phase increment the engine multiplies for and the oracle divides
 * for, over every frequency there is. The lockstep cannot stand in for
 * this: a constant that is wrong for a handful of frequencies is right
 * for all the ones a song happens to play, and the pairs this rejects
 * first fail at 53932 and 12307, which no test above ever sounds.
 *
 * Must match PHASE_MAGIC and PHASE_SHIFT in aud_psg.sv. */
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
    dut = new Vaud_psg;
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
