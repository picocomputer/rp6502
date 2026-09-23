/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_BENCH_TB_MACHINE_H_
#define _TESTS_BENCH_TB_MACHINE_H_

#include "tb_tcm.h"

#include <cstdint>
#include <string>
#include <vector>

template <typename Dut> static void tb_clock(Dut *dut)
{
    static bool rv_phase;
    static bool en = true;
    if (!dut->rst_n || !dut->wiring_sst_stop_req)
        en = true;
    else
        en = false;
    dut->mach_running = en;
    rv_phase = !rv_phase;
    dut->clk_rv = rv_phase;
    dut->clk_mach = en;
    dut->clk_sys = 1;
    dut->clk_a2 = 0;
    dut->eval();
    /* XRAM's port clock rises twice in each machine clock, clear of both
     * of its edges, and clk_ph tells the port which rise is which. */
    dut->clk_rv = 0;
    dut->clk_mach = 0;
    dut->clk_sys = 0;
    dut->clk_ph = 0;
    dut->clk_a2 = 1;
    dut->eval();
    dut->clk_a2 = 0;
    dut->eval();
    dut->clk_ph = 1;
    dut->clk_a2 = 1;
    dut->eval();
}

template <typename Dut>
static bool tb_firmware(Dut *dut, const char *path)
{
    auto *r = dut->rootp;
    return tb_load_tcm(r->wiring__DOT__soc__DOT__tcm0,
                       r->wiring__DOT__soc__DOT__tcm1,
                       r->wiring__DOT__soc__DOT__tcm2,
                       r->wiring__DOT__soc__DOT__tcm3, path);
}

template <typename Dut> static void tb_reset(Dut *dut)
{
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
        tb_clock(dut);
    dut->rst_n = 1;
}

#ifdef SW_BIN
#include "tb_host.h"
#include "tb_quiet.h"
#include "tb_stage.h"

template <typename Dut>
static void tb_platform_clock(Dut *dut, const std::vector<uint8_t> &rom,
                              std::string *console)
{
    uint32_t at = dut->wiring_stage_addr;
    tb_host_tick(dut, rom);
    dut->stage_half = tb_stage_half(rom, at);
    tb_clock(dut);
    if (console && dut->wiring_tx_valid)
        console->push_back((char)dut->wiring_tx_data);
}

template <typename Dut, typename Each>
static bool tb_run(Dut *dut, const std::vector<uint8_t> &rom,
                   std::string *console, Each each)
{
    return tb_quiet(dut, [&] {
        tb_platform_clock(dut, rom, console);
        each();
    });
}

template <typename Dut, typename Each>
static bool tb_boot_each(Dut *dut, const std::vector<uint8_t> &rom,
                         std::string *console, Each each)
{
    if (!tb_firmware(dut, SW_BIN))
        return false;
    tb_reset(dut);
    dut->rootp->wiring__DOT__soc__DOT__mmio_slot_len = (uint32_t)rom.size();
    return tb_run(dut, rom, console, each);
}

template <typename Dut>
static bool tb_boot(Dut *dut, const std::vector<uint8_t> &rom,
                    std::string *console = nullptr)
{
    return tb_boot_each(dut, rom, console, [] {});
}
#endif /* SW_BIN */

static uint32_t tb_rgba8(uint16_t px)
{
    uint32_t r5 = px & 0x1F;
    uint32_t g5 = (px >> 6) & 0x1F;
    uint32_t b5 = (px >> 11) & 0x1F;
    uint32_t r = (r5 << 3) | (r5 >> 2);
    uint32_t g = (g5 << 3) | (g5 >> 2);
    uint32_t b = (b5 << 3) | (b5 >> 2);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

template <typename Dut, typename Each>
static void tb_frame_start(Dut *dut, Each each)
{
    /* Already on a frame's first clock, which is where a boot that settled
     * returns; going round to 524 first would waste the whole frame. */
    if (dut->wiring_scanline == 0 && dut->rootp->wiring__DOT__vid_h == 0
        && !dut->rootp->wiring__DOT__timing__DOT__tick)
        return;
    while (dut->wiring_scanline != 524)
    {
        each();
        tb_clock(dut);
    }
    while (dut->wiring_scanline != 0)
    {
        each();
        tb_clock(dut);
    }
}

template <typename Dut, typename Each>
static void tb_capture_each(Dut *dut, uint32_t *fb, size_t px, Each each)
{
    tb_frame_start(dut, each);
    size_t at = 0;
    while (at < px)
    {
        each();
        tb_clock(dut);
        if (dut->wiring_vid_de)
            fb[at++] = tb_rgba8(dut->wiring_vid_pixel);
    }
}

template <typename Dut>
static void tb_capture(Dut *dut, uint32_t *fb, size_t px)
{
    tb_capture_each(dut, fb, px, [] {});
}

#endif /* _TESTS_BENCH_TB_MACHINE_H_ */
