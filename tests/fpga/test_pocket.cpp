/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The whole Pocket stack against the oracle: the host streams a real
 * .rp6502 over the bridge into SDRAM, the run gate releases the
 * machine, the firmware loads the ROM through the stalled staging
 * window, and the program paints. The frame the scaler receives —
 * decoded from vs/hs/de at 25.2 MHz and unpacked from RGB888 — must
 * be settled and pixel-exact against emu_core running the same file.
 */

#include "Vtb_pocket.h"
#include "Vtb_pocket___024root.h"

#include "oracle.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <vector>

static Vtb_pocket *dut;

static long a_next, s_next;
static long g_t, g_sys;

/* clk_sys period 165 with clk_vid on every fourth rise; clk_74a at
 * period 224 — the true PLL family against the bridge clock. */
static void tick()
{
    long next = a_next < s_next ? a_next : s_next;
    g_t = next;
    bool sedge = next == s_next;
    bool aedge = next == a_next;
    if (sedge)
    {
        dut->clk_sys = 1;
        if ((g_sys & 3) == 0)
            dut->clk_vid = 1;
    }
    if (aedge)
        dut->clk_74a = 1;
    dut->eval();
    if (sedge)
    {
        dut->clk_sys = 0;
        dut->clk_vid = 0;
        s_next += 165;
        g_sys++;
    }
    if (aedge)
    {
        dut->clk_74a = 0;
        a_next += 224;
    }
    dut->eval();
}

/* One clk_74a rising edge. */
static void a_edge()
{
    long t = a_next;
    while (a_next == t)
        tick();
}

static bool load_firmware(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    auto &tcm =
        dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__rv__DOT__tcm;
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

/* Capture one full decoded frame off the scaler interface: sample on
 * clk_vid rises, track the raster from vs, collect de pixels. */
static void capture_frame(uint32_t *fb)
{
    size_t at = 0;
    bool started = false;
    while (at < 640 * 480)
    {
        long sys_before = g_sys;
        tick();
        bool vid_rise = g_sys != sys_before && ((sys_before & 3) == 0);
        if (!vid_rise)
            continue;
        if (dut->tb_pocket_vs)
        {
            at = 0; /* a partial catch restarts at the origin */
            started = true;
            continue;
        }
        if (started && dut->tb_pocket_de)
            fb[at++] = dut->tb_pocket_rgb;
    }
}

/* The oracle's framebuffer is RGBA with the same replication the
 * adapter does; repack it into the scaler's RGB888 lanes. */
static uint32_t scaler_rgb(uint32_t rgba)
{
    uint32_t r = rgba & 0xFF;
    uint32_t g = (rgba >> 8) & 0xFF;
    uint32_t b = (rgba >> 16) & 0xFF;
    return (r << 16) | (g << 8) | b;
}

UTEST(pocket, full_stack_frame_matches_oracle)
{
    /* The oracle plays the same file. */
    ASSERT_TRUE(oracle_restart(TEST_FIXTURE));
    oracle_run_frames(30);
    int ow = 0, oh = 0;
    oracle_canvas_size(&ow, &oh);
    const uint32_t *ofb = oracle_framebuffer();

    std::vector<uint8_t> rom;
    {
        FILE *f = fopen(TEST_FIXTURE, "rb");
        ASSERT_TRUE(f != NULL);
        uint8_t buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            rom.insert(rom.end(), buf, buf + n);
        fclose(f);
    }

    /* Power on: platform reset with the host still holding Reset. */
    a_next = 224;
    s_next = 165;
    ASSERT_TRUE(load_firmware(SW_BIN));
    dut->rst_n = 0;
    dut->arst_n = 0;
    dut->reset_n = 0;
    for (int i = 0; i < 32; i++)
        tick();
    dut->rst_n = 1;
    dut->arst_n = 1;

    long guard = 0;
    while (!dut->tb_pocket_ready && guard++ < 60000)
        tick();
    ASSERT_TRUE(dut->tb_pocket_ready);

    /* The host boot: stream the slot, answer the table, complete,
     * release. */
    dut->datatable_q = (uint32_t)rom.size();
    for (size_t i = 0; i < rom.size(); i += 4)
    {
        uint32_t w = 0;
        for (size_t k = 0; k < 4; k++)
        {
            uint8_t b = i + k < rom.size() ? rom[i + k] : 0;
            w |= (uint32_t)b << (24 - 8 * k);
        }
        dut->bridge_wr = 1;
        dut->bridge_addr = (uint32_t)i;
        dut->bridge_wr_data = w;
        a_edge();
        dut->bridge_wr = 0;
        for (int k = 0; k < 39; k++)
            a_edge();
    }
    dut->dataslot_allcomplete = 1;
    a_edge();
    dut->dataslot_allcomplete = 0;
    dut->reset_n = 1;

    /* Boot, load through the stalled staging, render to settle: the
     * mode ROMs halt when their frame is up. */
    bool stopped = false;
    for (long i = 0; i < 60000000 && !stopped; i++)
    {
        tick();
        stopped = dut->tb_pocket_rv_halted
            && dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__cpu__DOT__stop_flag
                   != 0;
    }
    ASSERT_TRUE(stopped);

    /* Two consecutive scaler frames, settled and equal. */
    static uint32_t fb[2][640 * 480];
    capture_frame(fb[0]);
    capture_frame(fb[1]);
    ASSERT_EQ(memcmp(fb[0], fb[1], sizeof(fb[0])), 0);

    /* Pixel-exact against the oracle through the canvas mapping and
     * the RGB888 replication. */
    int xs = ow == 320 ? 1 : 0;
    int ys = (oh == 240 || oh == 180) ? 1 : 0;
    int yo = (oh == 180 || oh == 360) ? 60 : 0;
    int diffs = 0;
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++)
        {
            uint32_t want = 0;
            if (y >= yo && ((y - yo) >> ys) < oh)
                want = scaler_rgb(
                    ofb[(size_t)((y - yo) >> ys) * (size_t)ow
                        + (size_t)(x >> xs)]);
            if (fb[0][(size_t)y * 640 + x] != want)
            {
                if (getenv("POCKET_DEBUG") && diffs < 20)
                    fprintf(stderr, "diff x=%d y=%d rtl=%06X want=%06X\n",
                            x, y, fb[0][(size_t)y * 640 + x], want);
                diffs++;
            }
        }
    ASSERT_EQ(diffs, 0);

    /* The codec side is alive: LRCK ticks at audio rate. */
    int lrck_flips = 0;
    int lrck_q = dut->tb_pocket_lrck;
    for (int i = 0; i < 200000; i++)
    {
        tick();
        if ((int)dut->tb_pocket_lrck != lrck_q)
        {
            lrck_q = dut->tb_pocket_lrck;
            lrck_flips++;
        }
    }
    ASSERT_GT(lrck_flips, 50);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vtb_pocket;
    oracle_init();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
