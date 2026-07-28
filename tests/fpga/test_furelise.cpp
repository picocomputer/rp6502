/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * furelise.rp6502 on the whole machine: the ezpsg tracker programs the
 * PSG through xreg device 0 channel 1, pours note configs and gate
 * edges into XRAM through the RW engines, and paces itself on vsync.
 * The DSP is proven bit-exact by the psg lockstep; what the machine
 * adds — and what this test watches — is the plumbing: the xreg
 * dispatch through the soft CPU into the device register, the write
 * snoop that carries gate edges to the queue, the rotor feeding the
 * config fetch, and the 24 kHz sample grid against the 60 Hz frame.
 * The sample streams aren't compared to the emulator because it batches
 * a frame of samples after each frame's CPU run, so gate edges land at
 * different sample indices than the interleaved RTL. The second case
 * rings the console bell end to end: a typed ^G through the 6502's
 * echo loop, the sw's BEL scan at the sink, and the standing bell.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "tb_quiet.h"
#include "tb_tcm.h"
#include "utest.h"

#include <cstdio>
#include <vector>

static Vrp6502 *dut;
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
    dut->stage_rdata = a < rom.size() ? rom[a] : 0;
    dut->clk_sys = 1;
    dut->eval();
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

UTEST(furelise, plays_psg_audio)
{
    FILE *f = fopen(TEST_FIXTURE, "rb");
    ASSERT_TRUE(f != NULL);
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        rom.insert(rom.end(), buf, buf + n);
    fclose(f);

    ASSERT_TRUE(load_firmware(SW_BIN));
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
        clock_cycle();
    dut->rst_n = 1;
    dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();

    /* The standing bell walks from reset: the free-running 4,200-clock
     * grid lays exactly 400 samples into each 1,680,000-clock frame,
     * silent through the boot, the load, and the tracker's own lead-in
     * — around thirty-five frames of cadence proof before a note. */
    run_frame();
    int onset = -1;
    for (int i = 0; i < 80 && onset < 0; i++)
    {
        g_valids = 0;
        g_energy = 0;
        run_frame();
        ASSERT_EQ(g_valids, 400);
        if (g_energy > 0)
            onset = i;
    }
    ASSERT_GE(onset, 0);

    /* Eight frames of the opening phrase: the grid holds and the notes
     * are loud. Longer proves nothing the lockstep has not — energy
     * here runs half a million by the sixth frame. */
    g_energy = 0;
    g_peak = 0;
    for (int i = 0; i < 8; i++)
    {
        g_valids = 0;
        run_frame();
        ASSERT_EQ(g_valids, 400);
    }

    ASSERT_GT(g_peak, 32);
    ASSERT_GT(g_energy, 200000);
    ASSERT_EQ(dut->rootp->rp6502__DOT__cpu_run, 1);
}

/* No ROM staged: the built-in echo program. A typed ^G comes back out
 * the console sink, where the BEL scan strikes the teletype bell. */
UTEST(furelise, console_bel_rings)
{
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
        clock_cycle();
    dut->rst_n = 1;

    g_energy = 0;
    for (int i = 0; i < 4; i++)
        run_frame();
    ASSERT_EQ(g_energy, 0);

    dut->rootp->rp6502__DOT__rv__DOT__mmio_kbd_data = 0x07;
    dut->rootp->rp6502__DOT__rv__DOT__mmio_kbd_valid = 1;

    g_peak = 0;
    for (int i = 0; i < 4; i++)
    {
        g_valids = 0;
        run_frame();
        ASSERT_EQ(g_valids, 400);
    }
    ASSERT_GT(g_peak, 32);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vrp6502;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
