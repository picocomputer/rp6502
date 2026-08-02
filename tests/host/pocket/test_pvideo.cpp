/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * pocket_video against a replayed machine: the testbench emits the
 * beam's exact cadence — one de strobe per two system clocks, 640 per
 * line across 1,600 clocks, 480 lines then blanking, a frame pulse at
 * each origin — and decodes the scaler-side raster it gets back. Every
 * frame must carry one vs, a hs on all 525 lines never coincident with
 * vs, exactly 640x480 de pixels whose RGB888 replicate the RGB555
 * pattern, and black everywhere outside the window.
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
    dut->rst_n = 0;
    dut->vrst_n = 0;
    for (int i = 0; i < 4; i++)
    {
        dut->clk_sys = 1;
        dut->clk_vid = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->clk_vid = 0;
        dut->eval();
    }
    dut->rst_n = 1;
    dut->vrst_n = 1;
    dut->vid_canvas = 0; /* console: full 640x480 */

    long got_px = 0, want_px = 0;
    int vs_count = 0, hs_count = 0;
    int rx = -1, ry = -1; /* decoded raster position, from vs/hs */
    long de_run = 0;
    bool started = false;

    for (int f = 0; f < 3; f++)
    {
        for (int line = 0; line < 525; line++)
        {
            for (int c = 0; c < 1600; c++)
            {
                /* Machine side, one clk_sys. */
                dut->vid_frame = line == 0 && c == 0;
                bool de = line < 480 && c < 1280 && (c & 1) == 1;
                dut->vid_de = de;
                if (de)
                    dut->vid_pixel = pattern(f, c >> 1, line);
                dut->clk_sys = 1;
                if ((c & 1) == 0)
                    dut->clk_vid = 1;
                dut->eval();

                /* Scaler side, sampled on its rising edge. */
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
                        /* The frame index of this pixel: the reader
                         * lags the writer by under a line. */
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

                dut->clk_sys = 0;
                if ((c & 1) == 0)
                    dut->clk_vid = 0;
                dut->eval();
                if (de)
                    want_px++;
            }
        }
    }
    ASSERT_TRUE(started);

    /* Three frames in, the reader trails by less than a frame. */
    ASSERT_EQ(want_px, 3L * 640 * 480);
    ASSERT_GT(got_px, 2L * 640 * 480);
    ASSERT_EQ(vs_count, 3);
    ASSERT_GE(hs_count, 2 * 525);
}

/* A 320x180 canvas, decoded the way the scaler latches it. The machine
 * emits the canvas natively — 320 de strobes on the first line of each
 * doubled pair inside the letterbox window, nothing anywhere else — so
 * the bench feeds exactly that, and this stage is pure clock alignment:
 * each row goes out in the same 800-clock slot the machine sends it as
 * one hs, 320 pixels over 320 clocks, then 1280 clocks of porch to the
 * next row's hs. skip stays zero: nothing duplicated exists to skip.
 * The end-of-line word names scaler slot 3 after every row. The counts
 * are the claim: a duplicated or letterboxed pixel reaching the scaler
 * breaks the totals before any color is compared. */
UTEST(pvideo, canvas_native_de_skip_and_slot)
{
    dut->rst_n = 0;
    dut->vrst_n = 0;
    for (int i = 0; i < 4; i++)
    {
        dut->clk_sys = 1;
        dut->clk_vid = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->clk_vid = 0;
        dut->eval();
    }
    dut->rst_n = 1;
    dut->vrst_n = 1;
    dut->vid_canvas = 2; /* vga_320_180 */

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
                /* The machine's native de: rows on even lines inside the
                 * letterbox window, 320 strobes back to back. */
                bool row = line >= 60 && line < 420 && !(line & 1);
                bool de = row && c < 640 && (c & 1) == 1;
                dut->vid_de = de;
                if (de)
                    dut->vid_pixel = pattern(f, c >> 1, line);
                dut->clk_sys = 1;
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
                        /* Same slot the machine sends it: rows on even
                         * lines inside the letterbox window. */
                        bool dump = py >= 60 && py < 420 && !(py & 1);
                        bool win = px >= 0 && px < 320 && py < 480;
                        ASSERT_EQ(dut->pocket_video_de, win && dump);
                        ASSERT_EQ(dut->pocket_video_skip, 0);
                        /* hs at row periods: every other slot here. */
                        if (rx == 3)
                            ASSERT_EQ(dut->pocket_video_hs, !(py & 1));
                        if (dut->pocket_video_de)
                        {
                            int pf = (int)(latched / (320L * 180));
                            ASSERT_EQ(dut->pocket_video_rgb,
                                      rgb888(pattern(pf, px, py)));
                            latched++;
                        }
                        else if (de_q)
                        {
                            /* The cycle after de falls: the slot word. */
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

                dut->clk_sys = 0;
                if ((c & 1) == 0)
                    dut->clk_vid = 0;
                dut->eval();
            }
        }
    }
    ASSERT_TRUE(started);
    /* Two-plus full frames of 320x180, one slot word per active line. */
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
