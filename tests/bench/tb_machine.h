/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Starting the machine, which every test that has one does the same
 * way: the firmware into the TCM lanes, four clocks in reset, the
 * staged image announced, then clocks until the run goes quiet — with
 * the host answering its window and the staging store answering its
 * reads on every one of them.
 *
 * The two clocks are one network. clk_rv is half clk_sys and rises with
 * it, which is the PLL's shape rather than a divider's, so a test that
 * clocked them independently would be simulating a machine that does
 * not exist. That is why this is a function and not four lines a test
 * writes for itself.
 *
 * Templated on the model so the header can stay out of Verilator's way:
 * verilated.h and utest.h both define __STDC_FORMAT_MACROS, and a test
 * includes them in an order this file must not disturb.
 */

#ifndef _TESTS_BENCH_TB_MACHINE_H_
#define _TESTS_BENCH_TB_MACHINE_H_

#include "tb_tcm.h"

#include <cstdint>
#include <string>
#include <vector>

/* One clk_sys cycle, with clk_rv taking its half-rate edge on the
 * rising one. The phase is a function-local static because there is one
 * machine in a test process, the way there is one in a chip. */
template <typename Dut> static void tb_clock(Dut *dut)
{
    static bool rv_phase;
    /* The clock gate, which on hardware is a control block at the
     * source. A savestate takes the machine's clock away entirely --
     * atomically, wherever the machine is -- and the soft CPU's clock
     * is not part of it: that core keeps its clock and is halted at
     * its debug port. The arrays and the serializer are on the clocks
     * that are left, which is how state gets read out of a machine
     * that is not running. Reset reopens the gate, so a test that
     * dies mid-save cannot poison the next one. */
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
    dut->eval();
    dut->clk_rv = 0;
    dut->clk_mach = 0;
    dut->clk_sys = 0;
    dut->eval();
}

/* The soft CPU's code memory, four byte lanes wide. */
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
/* The platform half, which only a firmware test has: staging assets and
 * a host to answer for them. SW_BIN comes with rp6502_add_machine_test's
 * FIRMWARE keyword, and so do the asset paths tb_stage.h names — a test
 * without it drives the machine's pins directly and has no business
 * pulling in a font image it will never stage. */
#include "tb_host.h"
#include "tb_quiet.h"
#include "tb_stage.h"

/* One clock with the platform around it: the host answering its
 * register window, the staging store answering the read the machine
 * put out, and the console byte if one moved. */
template <typename Dut>
static void tb_platform_clock(Dut *dut, const std::vector<uint8_t> &rom,
                              std::string *console)
{
    uint32_t at = dut->wiring_stage_addr;
    tb_host_tick(dut, rom);
    dut->stage_rdata = tb_stage(rom, at);
    tb_clock(dut);
    if (console && dut->wiring_tx_valid)
        console->push_back((char)dut->wiring_tx_data);
}

/* Run the announced image until the run goes quiet. `each` is called
 * after every clock, for the tests that watch something the machine
 * does not put on the console. */
template <typename Dut, typename Each>
static bool tb_run(Dut *dut, const std::vector<uint8_t> &rom,
                   std::string *console, Each each)
{
    return tb_quiet(dut, [&] {
        tb_platform_clock(dut, rom, console);
        each();
    });
}

/* Firmware in, reset released, image announced, run. */
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

/* rgb555_to_rgba8, byte for byte the emulator's (core/vga/vga.c). Both
 * framebuffers are compared word for word, so this has to be the same
 * arithmetic and not merely the same colors. */
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

/* Walk to the top of the next frame. */
template <typename Dut, typename Each>
static void tb_frame_start(Dut *dut, Each each)
{
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

/* One frame, from the top. The machine's de is the canvas — each pixel
 * once, each row once, no letterbox — so a frame is exactly
 * canvas-many pixels and the count is the only sync needed. `each` runs
 * before every clock, for a test holding something still while the beam
 * passes.
 *
 * The walk to the top comes first, and the frame it spends is not
 * waste. tb_run returns on the clock that starts a frame, so a capture
 * that began there would take the frame immediately after the run — and
 * prog latches the canvas at scanline 524, one line before the
 * beam's frame, so a program whose last xreg landed after that point
 * renders its old canvas for one more frame. The picture a stopped
 * program leaves is the frame after that one. mode3_2bppr is the
 * fixture that proves it: it finishes inside a single frame, and
 * capturing from the boundary tb_run returns on gives two frames that
 * differ. */
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
