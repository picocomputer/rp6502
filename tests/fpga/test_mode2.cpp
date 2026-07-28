/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 2 tile maps against the oracle: every depth, both tile sizes,
 * trimmed tiles, wraparound, builtin and XRAM palettes — and the
 * composite case stacking a mode 3 base under mode 2 tiles and mode 1
 * cells across all three planes. The ROMs come from tests/roms — the
 * corpus vidmodes.py generates and the emulator suite also boots — so
 * both machines run the identical file and settle. The comparison maps
 * the RTL's doubled and letterboxed raster onto the oracle's
 * canvas-native frame.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "oracle.h"
#include "tb_quiet.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vrp6502 *dut;

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
    dut->clk_sys = 1;
    dut->eval();
    dut->clk_sys = 0;
    dut->eval();
}

static void capture_frame(uint32_t *fb)
{
    while (dut->rp6502_scanline != 524)
        clock_cycle();
    while (dut->rp6502_scanline != 0)
        clock_cycle();
    size_t at = 0;
    while (at < 640 * 480)
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
        dut->stage_rdata = a < rom.size() ? rom[a] : 0;
        clock_cycle();
    }));

    static uint32_t fb[2][640 * 480];
    capture_frame(fb[0]);
    capture_frame(fb[1]);
    ASSERT_EQ(memcmp(fb[0], fb[1], sizeof(fb[0])), 0);

    int xs = ow == 320 ? 1 : 0;
    int ys = (oh == 240 || oh == 180) ? 1 : 0;
    int yo = (oh == 180 || oh == 360) ? 60 : 0;
    int diffs = 0;
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++)
        {
            uint32_t want;
            int cy = (y - yo) >> ys;
            if (y < yo || cy >= oh)
                want = 0xFF000000u;
            else
                want = ofb[(size_t)cy * ow + (x >> xs)];
            if (fb[0][(size_t)y * 640 + x] != want)
            {
                if (getenv("MODE2_DEBUG") && diffs < 20)
                    fprintf(stderr, "%s diff x=%d y=%d rtl=%08X want=%08X\n",
                            name, x, y, fb[0][(size_t)y * 640 + x], want);
                diffs++;
            }
        }
    ASSERT_EQ(diffs, 0);
}

UTEST(mode2, bpp1_8px_builtin_640x480)
{
    run_case(utest_result, "mode2_1bpp8");
}

UTEST(mode2, bpp2_16px_xram_palette_320x240)
{
    run_case(utest_result, "mode2_2bpp16");
}

UTEST(mode2, bpp4_8px_trimmed_320x240)
{
    run_case(utest_result, "mode2_4bpp8trim");
}

UTEST(mode2, bpp8_16px_wrap_320x180)
{
    run_case(utest_result, "mode2_8bpp16wrap");
}

UTEST(mode2, composite_three_planes_320x240)
{
    run_case(utest_result, "mode2_composite");
}

UTEST(mode2, bpp2_16px_both_trims_320x240)
{
    run_case(utest_result, "mode2_16trim");
}

UTEST(mode2, bpp4_8px_xtrim_640x480)
{
    run_case(utest_result, "mode2_trimx");
}

UTEST(mode2, bpp8_8px_xtrim_320x180)
{
    run_case(utest_result, "mode2_trimx8");
}

UTEST(mode2, bpp1_8px_ytrim_320x180)
{
    run_case(utest_result, "mode2_trimy");
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
