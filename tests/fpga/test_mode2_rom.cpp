/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The real mode2.rp6502 driven to completion on both machines: a random
 * 40x30 tilemap scrolls under vsync, a key press and release ends each
 * of its two scroll loops, and the program prints and exits. The random
 * seed is pinned to the firmware's, and the key events land the same
 * number of frames after the keyboard bitmap first appears in XRAM —
 * the anchor both machines share — so the scroll stops in the same
 * place. The final frame, the machine halted by EXIT with the display
 * still scanning the last tile field, compares pixel-exact.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "oracle.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <vector>

static Vrp6502 *dut;
static std::vector<uint8_t> rom;

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
    uint32_t a = dut->rp6502_stage_addr;
    dut->stage_rdata = a < rom.size() ? rom[a] : 0;
    dut->clk_sys = 1;
    dut->eval();
    dut->clk_sys = 0;
    dut->eval();
}

static void run_frame()
{
    while (dut->rp6502_scanline != 524)
        clock_cycle();
    while (dut->rp6502_scanline != 0)
        clock_cycle();
}

static void capture_frame(uint32_t *fb)
{
    run_frame();
    size_t at = 0;
    while (at < 640 * 480)
    {
        clock_cycle();
        if (dut->rp6502_vid_de)
            fb[at++] = rgba8(dut->rp6502_vid_pixel);
    }
}

static uint8_t rtl_xram(uint16_t addr)
{
    uint32_t w = dut->rootp->rp6502__DOT__xram__DOT__mem[addr >> 2];
    return (uint8_t)(w >> ((addr & 3) * 8));
}

static void rtl_key(uint8_t code, bool down)
{
    dut->rootp->rp6502__DOT__rv__DOT__mmio_key_data =
        (uint16_t)((down ? 0x100 : 0) | code);
    dut->rootp->rp6502__DOT__rv__DOT__mmio_key_valid = 1;
}

/* The keyboard bitmap's idle flag at its mapped address is the anchor:
 * the program maps it right before the first scroll loop. */
#define KBD_XRAM 0xFF10

UTEST(mode2_rom, key_driven_run_to_exit)
{
    FILE *f = fopen(TEST_FIXTURE, "rb");
    ASSERT_TRUE(f != NULL);
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        rom.insert(rom.end(), buf, buf + n);
    fclose(f);

    /* Oracle. */
    oracle_seed(1);
    ASSERT_TRUE(oracle_restart(TEST_FIXTURE));
    int anchor = -1;
    for (int i = 0; i < 60 && anchor < 0; i++)
    {
        oracle_run_frames(1);
        if (oracle_xram(KBD_XRAM) & 1)
            anchor = i;
    }
    ASSERT_GE(anchor, 0);
    oracle_run_frames(20);
    oracle_key(0x2C, true);
    oracle_run_frames(5);
    oracle_key(0x2C, false);
    oracle_run_frames(10);
    ASSERT_FALSE(oracle_halted()); /* second scroll loop */
    oracle_key(0x2C, true);
    oracle_run_frames(5);
    oracle_key(0x2C, false);
    for (int i = 0; i < 60 && !oracle_halted(); i++)
        oracle_run_frames(1);
    ASSERT_TRUE(oracle_halted());
    oracle_run_frames(2);
    int ow = 0, oh = 0;
    oracle_canvas_size(&ow, &oh);
    ASSERT_EQ(ow, 320);
    ASSERT_EQ(oh, 240);
    const uint32_t *ofb = oracle_framebuffer();

    /* RTL, the same cadence from the same anchor. */
    ASSERT_TRUE(load_firmware(SW_BIN));
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
        clock_cycle();
    dut->rst_n = 1;
    dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();

    anchor = -1;
    for (int i = 0; i < 120 && anchor < 0; i++)
    {
        run_frame();
        if (rtl_xram(KBD_XRAM) & 1)
            anchor = i;
    }
    ASSERT_GE(anchor, 0);
    for (int i = 0; i < 20; i++)
        run_frame();
    rtl_key(0x2C, true);
    for (int i = 0; i < 5; i++)
        run_frame();
    rtl_key(0x2C, false);
    for (int i = 0; i < 10; i++)
        run_frame();
    rtl_key(0x2C, true);
    for (int i = 0; i < 5; i++)
        run_frame();
    rtl_key(0x2C, false);

    bool done = false;
    for (int i = 0; i < 40000000 && !done; i++)
    {
        clock_cycle();
        done = dut->rp6502_rv_halted && !dut->rootp->rp6502__DOT__cpu_run;
    }
    ASSERT_TRUE(done);

    static uint32_t fb[2][640 * 480];
    capture_frame(fb[0]);
    capture_frame(fb[1]);
    ASSERT_EQ(memcmp(fb[0], fb[1], sizeof(fb[0])), 0);

    int diffs = 0;
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++)
        {
            uint32_t want = ofb[(size_t)(y >> 1) * 320 + (x >> 1)];
            if (fb[0][(size_t)y * 640 + x] != want)
            {
                if (getenv("MODE2_ROM_DEBUG") && diffs < 20)
                    fprintf(stderr, "diff x=%d y=%d rtl=%08X want=%08X\n",
                            x, y, fb[0][(size_t)y * 640 + x], want);
                diffs++;
            }
        }
    ASSERT_EQ(diffs, 0);
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
