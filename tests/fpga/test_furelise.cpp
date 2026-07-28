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
 * different sample indices than the interleaved RTL.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

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
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    auto &tcm = dut->rootp->rp6502__DOT__rv__DOT__tcm;
    for (size_t i = 0; i < 32768; i++)
        tcm[i] = 0;
    uint8_t buf[4];
    size_t word = 0, n;
    while ((n = fread(buf, 1, 4, f)) > 0 && word < 32768)
    {
        uint32_t v = 0;
        for (size_t i = 0; i < n; i++)
            v |= (uint32_t)buf[i] << (8 * i);
        tcm[word++] = v;
    }
    fclose(f);
    return true;
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

    /* Boot and load until ezpsg_init lands the block pointer — the
     * first sample walk marks the PSG live. */
    int onset = -1;
    for (int i = 0; i < 150 && onset < 0; i++)
    {
        g_valids = 0;
        run_frame();
        if (g_valids > 0)
            onset = i;
    }
    ASSERT_GE(onset, 0);

    /* From the next frame boundary the free-running 4,200-clock grid
     * lays exactly 400 samples into each 1,680,000-clock frame. */
    g_energy = 0;
    g_peak = 0;
    for (int i = 0; i < 90; i++)
    {
        g_valids = 0;
        run_frame();
        ASSERT_EQ(g_valids, 400);
    }

    /* A second and a half of Beethoven: audibly loud, sustained, and
     * still playing. */
    ASSERT_GT(g_peak, 32);
    ASSERT_GT(g_energy, 100000);
    ASSERT_FALSE(dut->rp6502_rv_halted);
    ASSERT_EQ(dut->rootp->rp6502__DOT__cpu_run, 1);
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
