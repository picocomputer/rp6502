/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vpocket_video.h"

#include "utest.h"

#include <cstdint>

static Vpocket_video *dut;

static uint16_t pattern(int f, int x, int y)
{
    uint32_t h = (uint32_t)(f * 640 * 480 + y * 640 + x);
    h = h * 2654435761u;
    return (uint16_t)(h >> 13);
}

static uint32_t rgb888(uint16_t p)
{
    uint32_t r = ((p & 0x1F) << 3) | ((p >> 2) & 7);
    uint32_t g = (((p >> 6) & 0x1F) << 3) | ((p >> 8) & 7);
    uint32_t b = (((p >> 11) & 0x1F) << 3) | ((p >> 13) & 7);
    return (r << 16) | (g << 8) | b;
}

UTEST(pvideo, raster_and_pixels_exact)
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vpocket_video;
    dut->run = 1;
    dut->clk_mach = 0;
    dut->clk_vid = 0;
    dut->eval();
    for (int i = 0; i < 4; i++)
    {
        dut->clk_mach = 1;
        dut->clk_vid = 1;
        dut->eval();
        dut->clk_mach = 0;
        dut->clk_vid = 0;
        dut->eval();
    }
    dut->vid_canvas = 0; /* vga_canvas_console */

    long got_px = 0, want_px = 0;
    int vs_count = 0, hs_count = 0;
    int rx = -1, ry = -1;
    long de_run = 0;
    bool started = false;

    for (int f = 0; f < 3; f++)
    {
        for (int line = 0; line < 525; line++)
        {
            for (int c = 0; c < 1600; c++)
            {
                dut->vid_frame = line == 0 && c == 0;
                bool de = line < 480 && c < 1280 && (c & 1) == 1;
                dut->vid_de = de;
                if (de)
                    dut->vid_pixel = pattern(f, c >> 1, line);
                dut->clk_mach = 1;
                if ((c & 1) == 0)
                    dut->clk_vid = 1;
                dut->eval();

                if ((c & 1) == 0)
                {
                    if (dut->pocket_video_vs)
                    {
                        ASSERT_FALSE(dut->pocket_video_hs);
                        vs_count++;
                        rx = 0;
                        ry = 0;
                        started = true;
                    }
                    else if (rx >= 0 && ++rx == 800)
                    {
                        rx = 0;
                        ry++;
                    }
                    if (dut->pocket_video_hs)
                    {
                        ASSERT_EQ(rx, 3);
                        hs_count++;
                    }
                    if (dut->pocket_video_de)
                    {
                        int px = rx - 9;
                        int py = ry;
                        ASSERT_GE(px, 0);
                        ASSERT_LT(px, 640);
                        ASSERT_LT(py, 480);
                        int pf = (int)(got_px / (640L * 480));
                        ASSERT_EQ(dut->pocket_video_rgb,
                                  rgb888(pattern(pf, px, py)));
                        got_px++;
                        de_run++;
                    }
                    else
                    {
                        ASSERT_EQ(dut->pocket_video_rgb, 0u);
                        if (de_run)
                            ASSERT_EQ(de_run, 640L);
                        de_run = 0;
                    }
                    ASSERT_EQ(dut->pocket_video_skip, 0);
                }

                dut->clk_mach = 0;
                if ((c & 1) == 0)
                    dut->clk_vid = 0;
                dut->eval();
                if (de)
                    want_px++;
            }
        }
    }
    ASSERT_TRUE(started);

    ASSERT_EQ(want_px, 3L * 640 * 480);
    ASSERT_GT(got_px, 2L * 640 * 480);
    ASSERT_EQ(vs_count, 3);
    ASSERT_GE(hs_count, 2 * 525);
}

UTEST(pvideo, canvas_native_de_skip_and_slot)
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vpocket_video;
    dut->run = 1;
    dut->clk_mach = 0;
    dut->clk_vid = 0;
    dut->eval();
    for (int i = 0; i < 4; i++)
    {
        dut->clk_mach = 1;
        dut->clk_vid = 1;
        dut->eval();
        dut->clk_mach = 0;
        dut->clk_vid = 0;
        dut->eval();
    }
    dut->vid_canvas = 2; /* vga_canvas_320_180 */

    long latched = 0, endlines = 0;
    int rx = -1, ry = -1;
    bool started = false, de_q = false;

    for (int f = 0; f < 3; f++)
    {
        for (int line = 0; line < 525; line++)
        {
            for (int c = 0; c < 1600; c++)
            {
                dut->vid_frame = line == 0 && c == 0;
                bool row = !(line & 1) && line < 360;
                bool de = row && c < 640 && (c & 1) == 1;
                dut->vid_de = de;
                if (de)
                    dut->vid_pixel = pattern(f, c >> 1, line >> 1);
                dut->clk_mach = 1;
                if ((c & 1) == 0)
                    dut->clk_vid = 1;
                dut->eval();

                if ((c & 1) == 0)
                {
                    if (dut->pocket_video_vs)
                    {
                        rx = 0;
                        ry = 0;
                        started = true;
                    }
                    else if (rx >= 0 && ++rx == 800)
                    {
                        rx = 0;
                        ry++;
                    }
                    if (started)
                    {
                        int px = rx - 9;
                        int py = ry;
                        bool dump = !(py & 1) && py < 360;
                        bool win = px >= 0 && px < 320 && py < 480;
                        ASSERT_EQ(dut->pocket_video_de, win && dump);
                        ASSERT_EQ(dut->pocket_video_skip, 0);
                        /* HS delimits a scanline, so the lines in between
                         * the handed-over rows carry none. */
                        if (rx == 3)
                            ASSERT_EQ(dut->pocket_video_hs,
                                      !((py & 1) && py < 360));
                        if (dut->pocket_video_de)
                        {
                            int pf = (int)(latched / (320L * 180));
                            ASSERT_EQ(dut->pocket_video_rgb,
                                      rgb888(pattern(pf, px, py >> 1)));
                            latched++;
                        }
                        else if (de_q)
                        {
                            /* On the first cycle after de falls, rgb holds
                             * the scaler slot in bits 23:13, and the 320x180
                             * entry in video.json's scaler_modes is slot 3. */
                            ASSERT_EQ(dut->pocket_video_rgb,
                                      (uint32_t)3 << 13);
                            endlines++;
                        }
                        else
                        {
                            ASSERT_EQ(dut->pocket_video_rgb, 0u);
                        }
                        de_q = dut->pocket_video_de;
                    }
                }

                dut->clk_mach = 0;
                if ((c & 1) == 0)
                    dut->clk_vid = 0;
                dut->eval();
            }
        }
    }
    ASSERT_TRUE(started);
    ASSERT_GT(latched, 2L * 320 * 180);
    ASSERT_GT(endlines, 2L * 180);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vpocket_video;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
