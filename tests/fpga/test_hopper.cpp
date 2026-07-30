/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A real program, on the whole machine, for as long as it takes to see
 * what it does. RPStarHopper is a megabyte of ROM with bundled assets
 * behind a "#>" program header, which makes it the first thing here to
 * exercise the loader's asset path, the staging window under a running
 * program, and the video engines at the rate a game drives them.
 *
 * Nothing about the picture is asserted. What this watches is both
 * consoles and the machine's own liveness — a program that stops, a
 * loader that rejects, or a renderer that misses the beam all say so
 * without anyone comparing pixels.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "tb_stage.h"
#include "tb_tcm.h"
#include "utest.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vrp6502 *dut;
static bool rv_phase;
static std::vector<uint8_t> rom;
static std::string cpu_out, rv_out;
static long g_clocks;
static long g_valids, g_energy;
static int g_peak;

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
    g_clocks++;
    if (dut->rp6502_tx_valid)
        cpu_out.push_back((char)dut->rp6502_tx_data);
    if (dut->rp6502_rv_tx_valid)
        rv_out.push_back((char)dut->rp6502_rv_tx_data);
    if (dut->rp6502_aud_valid)
    {
        g_valids++;
        int d = (int)dut->rp6502_aud_l - 512;
        if (d < 0)
            d = -d;
        g_energy += d;
        if (d > g_peak)
            g_peak = d;
    }
}

static void run_frame()
{
    while (dut->rp6502_scanline != 524)
        clock_cycle();
    while (dut->rp6502_scanline != 0)
        clock_cycle();
}

UTEST(hopper, runs_far_enough_to_see)
{
    FILE *f = fopen(HOPPER_ROM, "rb");
    ASSERT_TRUE(f != NULL);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        rom.insert(rom.end(), buf, buf + n);
    fclose(f);
    printf("rom %zu bytes\n", rom.size());

    auto *r = dut->rootp;
    ASSERT_TRUE(tb_load_tcm(r->rp6502__DOT__rv__DOT__tcm0,
                            r->rp6502__DOT__rv__DOT__tcm1,
                            r->rp6502__DOT__rv__DOT__tcm2,
                            r->rp6502__DOT__rv__DOT__tcm3, SW_BIN));
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
        clock_cycle();
    dut->rst_n = 1;
    r->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();

    const int FRAMES = 120;
    auto t0 = std::chrono::steady_clock::now();
    int ran = 0;
    for (; ran < FRAMES; ran++)
    {
        run_frame();
        if (r->rp6502__DOT__cpu__DOT__stop_flag)
            break;
    }
    double secs = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - t0).count();

    printf("%d frames in %.1fs (%.2f Mclk/s), %zu clocks\n", ran, secs,
           g_clocks / secs / 1e6, (size_t)g_clocks);
    printf("audio: %ld samples, energy %ld, peak %d of 511\n",
           g_valids, g_energy, g_peak);
    printf("sprite overrun %u  cpu_run %d  stp %d\n",
           (unsigned)r->rp6502__DOT__vid_sprite__DOT__vid_sprite_overrun,
           (int)r->rp6502__DOT__cpu_run,
           (int)r->rp6502__DOT__cpu__DOT__stop_flag);
    printf("--- soft cpu (%zu) ---\n%s\n", rv_out.size(),
           rv_out.substr(rv_out.size() > 600 ? rv_out.size() - 600 : 0).c_str());
    printf("--- 6502 (%zu) ---\n%s\n", cpu_out.size(),
           cpu_out.substr(cpu_out.size() > 400 ? cpu_out.size() - 400 : 0).c_str());

    /* It has to have got past the loader and be running. */
    ASSERT_TRUE(rv_out.find("boot: running") != std::string::npos);
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
