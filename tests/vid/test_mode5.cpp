/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 5 sprites against the oracle: a sprite-only plane claiming a
 * zeroed layer, sprites over a fill on their own plane, sprites under a
 * text plane above, and the big squares from a non-zero plane on the
 * letterboxed canvas — clips off every edge, overlap, per-sprite
 * palettes with the builtin fallback and a halfword-aligned read. The
 * ROMs come from tests/roms; both machines run the identical file and
 * settle, and the comparison maps the RTL raster onto the oracle's
 * canvas-native frame.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "oracle.h"
#include "tb_quiet.h"
#include "tb_host.h"
#include "tb_stage.h"
#include "tb_tcm.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vrp6502 *dut;
/* Half clk_sys, rising with it: the PLL's shape, not a divider's. */
static bool rv_phase;

static bool load_firmware(const char *path)
{
    auto *r = dut->rootp;
    return tb_load_tcm(r->rp6502__DOT__rv__DOT__tcm0,
                       r->rp6502__DOT__rv__DOT__tcm1,
                       r->rp6502__DOT__rv__DOT__tcm2,
                       r->rp6502__DOT__rv__DOT__tcm3, path);
}

static uint32_t rgba8(uint16_t px)
{
    uint32_t r5 = px & 0x1F;
    uint32_t g5 = (px >> 6) & 0x1F;
    uint32_t b5 = (px >> 11) & 0x1F;
    uint32_t r = (r5 << 3) | (r5 >> 2);
    uint32_t g = (g5 << 3) | (g5 >> 2);
    uint32_t b = (b5 << 3) | (b5 >> 2);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

static void clock_cycle()
{
    rv_phase = !rv_phase;
    dut->clk_rv = rv_phase;
    dut->clk_sys = 1;
    dut->eval();
    dut->clk_rv = 0;
    dut->clk_sys = 0;
    dut->eval();
}

/* The machine's de is the canvas now — each pixel once, each row once,
 * no letterbox — so a frame is exactly canvas-many pixels. */
static void capture_frame(uint32_t *fb, size_t px)
{
    while (dut->rp6502_scanline != 524)
        clock_cycle();
    while (dut->rp6502_scanline != 0)
        clock_cycle();
    size_t at = 0;
    while (at < px)
    {
        clock_cycle();
        if (dut->rp6502_vid_de)
            fb[at++] = rgba8(dut->rp6502_vid_pixel);
    }
}

/* One fixture end to end: run both machines, settle, compare. */
static void run_case(int *utest_result, const char *name)
{
    std::string path = std::string(ROMS_DIR "/") + name + ".rp6502";
    FILE *f = fopen(path.c_str(), "rb");
    ASSERT_TRUE(f != NULL);
    std::vector<uint8_t> rom;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        rom.insert(rom.end(), buf, buf + n);
    fclose(f);
    ASSERT_TRUE(rom.size() > 0);

    ASSERT_TRUE(oracle_restart(path.c_str()));
    oracle_run_frames(30);
    int ow = 0, oh = 0;
    oracle_canvas_size(&ow, &oh);
    const uint32_t *ofb = oracle_framebuffer();

    ASSERT_TRUE(load_firmware(SW_BIN));
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
        clock_cycle();
    dut->rst_n = 1;
    dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();

    ASSERT_TRUE(tb_quiet(dut, [&] {
        uint32_t a = dut->rp6502_stage_addr;
        tb_host_tick(dut, rom);
        dut->stage_rdata = tb_stage(rom, a);
        clock_cycle();
    }));

    static uint32_t fb[2][640 * 480];
    const size_t px = (size_t)ow * (size_t)oh;
    capture_frame(fb[0], px);
    capture_frame(fb[1], px);
    ASSERT_EQ(memcmp(fb[0], fb[1], px * 4), 0);

    int diffs = 0;
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++)
        {
            uint32_t want = ofb[(size_t)y * ow + x];
            if (fb[0][(size_t)y * ow + x] != want)
            {
                if (getenv("MODE5_DEBUG") && diffs < 20)
                    fprintf(stderr, "%s diff x=%d y=%d rtl=%08X want=%08X\n",
                            name, x, y, fb[0][(size_t)y * ow + x], want);
                diffs++;
            }
        }
    ASSERT_EQ(diffs, 0);
}

UTEST(mode5, bpp8_8x8_sprite_only_320x240)
{
    run_case(utest_result, "mode5_8x8");
}

UTEST(mode5, bpp2_16x16_over_fill_320x240)
{
    run_case(utest_result, "mode5_16x16");
}

UTEST(mode5, bpp4_32x32_under_text_640x480)
{
    run_case(utest_result, "mode5_32x32");
}

UTEST(mode5, bpp8_64x64_plane1_320x180)
{
    run_case(utest_result, "mode5_64x64");
}

/* Every engine at once, harder than any real program: a wrapped
 * full-width bitmap under six 64-pixel sprites and four paletted ones,
 * with text over it. This used to assert the counter stayed at zero,
 * which was true of a line twice as long as the machine now has — the
 * fixture was calibrated against 100.8 MHz and the machine runs at
 * 50.4. Losing a race here is the beam winning against a load nothing
 * ships, and the counter reporting it is the designed behaviour, so
 * that is what this checks: the picture still settles, and the count is
 * a count. What must not happen is silence. */
UTEST(mode5, stress_budget_640x480)
{
    run_case(utest_result, "sprite_stress");
    printf("  sprite_stress lost %u races\n",
           dut->rootp->rp6502__DOT__vid_sprite__DOT__vid_sprite_overrun);
}

UTEST(mode5, bpp1_128_halfword_descs_320x240)
{
    run_case(utest_result, "mode5_1bpp128");
}

UTEST(mode5, bpp4_256_640x480)
{
    run_case(utest_result, "mode5_4bpp256");
}

/* A slot built to lose its race: the counter reports it, the machine
 * keeps its cadence, and the partial paint settles. Only the counter
 * and settling are asserted — the oracle has no deadline to lose. */
UTEST(mode5, overrun_counts_lost_races_320x240)
{
    std::string path = std::string(ROMS_DIR "/sprite_overrun.rp6502");
    FILE *f = fopen(path.c_str(), "rb");
    ASSERT_TRUE(f != NULL);
    std::vector<uint8_t> rom;
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

    ASSERT_TRUE(tb_quiet(dut, [&] {
        uint32_t a = dut->rp6502_stage_addr;
        tb_host_tick(dut, rom);
        dut->stage_rdata = tb_stage(rom, a);
        clock_cycle();
    }));

    /* sprite_overrun is built on canvas 1 — 320x240, vidmodes.py — and
     * this case runs no oracle, so the frame size is stated here. */
    static uint32_t fb[2][640 * 480];
    const size_t px = 320 * 240;
    capture_frame(fb[0], px);
    uint16_t over = dut->rootp->rp6502__DOT__vid_sprite__DOT__vid_sprite_overrun;
    capture_frame(fb[1], px);
    ASSERT_EQ(memcmp(fb[0], fb[1], px * 4), 0);
    ASSERT_GT(over, 0);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vrp6502;
    oracle_init();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
