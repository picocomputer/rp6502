/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * mister_video behind mister_bars, read the way sys/ reads emu's video:
 * a pixel on every clock with CE_PIXEL high. The raster is found by its
 * first VS and from there every pixel is checked: 800 pixels a line, 525
 * lines, HS on pixels 656-751, VS on lines 490-491, DE on the canvas, the
 * colour, the aspect ratio, and a 320 wide canvas's rows on two lines.
 */

#include "Vtb_mvideo.h"

#include "utest.h"

#include <cstdint>
#include <cstdio>

static Vtb_mvideo *dut;

static void fresh(uint8_t canvas, bool frame_en)
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vtb_mvideo;
    dut->clk_sys = 0;
    dut->canvas = canvas;
    dut->frame_en = frame_en;
    dut->eval();
}

static void tick()
{
    dut->clk_sys = 1;
    dut->eval();
    dut->clk_sys = 0;
    dut->eval();
}

static int canvas_w(int cv)
{
    return cv == 1 || cv == 2 || cv == 5 ? 320 : 640;
}

static int canvas_h(int cv)
{
    switch (cv)
    {
    case 1:
        return 240;
    case 2:
        return 180;
    case 4:
        return 360;
    case 5:
        return 200;
    case 6:
        return 400;
    default:
        return 480;
    }
}

/* mister_bars's card, an RGB555 pixel with alpha in bit 5. */
static uint16_t bars(int cv, int col, int row)
{
    bool dbl = canvas_w(cv) == 320;
    int bar = ((dbl ? col << 1 : col) / 80) & 7;
    if (row < canvas_h(cv) / 2)
        return (uint16_t)((bar & 4 ? 0x1F << 11 : 0) | (bar & 2 ? 0x1F << 6 : 0) |
                          (bar & 1 ? 0x1F : 0));
    return (uint16_t)(((row & 0x1F) << 11) | (((col >> 4) & 0x1F) << 6) |
                      ((col & 1) << 5) | (col & 0x1F));
}

static uint32_t rgb888(uint16_t p)
{
    uint32_t r = ((p & 0x1F) << 3) | ((p >> 2) & 7);
    uint32_t g = (((p >> 6) & 0x1F) << 3) | ((p >> 8) & 7);
    uint32_t b = (((p >> 11) & 0x1F) << 3) | ((p >> 13) & 7);
    return (r << 16) | (g << 8) | b;
}

/* Follows the output from its first VS. cv and live are what the frame
 * being shown should have; next_cv and next_live take over at the next
 * frame. */
struct Raster
{
    bool started = false;
    bool ce_q = false;
    bool vs_q = false;
    int x = 0, y = 0;
    long clocks = 0;
    int cv = 0, next_cv = 0;
    bool live = false, next_live = false;
    long frames = 0, done = 0, lit = 0, de_px = 0;
    uint32_t prev_line[640] = {};
    uint32_t line[640] = {};

    bool fail(const char *what, long got, long want)
    {
        printf("mvideo: %s at frame %ld y=%d x=%d: got %ld, want %ld\n", what,
               frames, y, x, got, want);
        return false;
    }

    /* One clock of output, after its rising edge. */
    bool clock()
    {
        bool ce = dut->tb_mvideo_ce;
        clocks++;
        if (started && ce == ce_q)
            return fail("CE_PIXEL not every other clock", ce, !ce_q);
        ce_q = ce;
        if (!ce)
            return true;
        if (!started)
        {
            if (!dut->tb_mvideo_vs || vs_q)
            {
                vs_q = dut->tb_mvideo_vs;
                return true;
            }
            started = true;
            x = 0;
            y = 490;
        }
        else if (++x == 800)
        {
            x = 0;
            for (int i = 0; i < 640; i++)
                prev_line[i] = line[i];
            if (++y == 525)
            {
                y = 0;
                frames++;
                cv = next_cv;
                live = next_live;
            }
        }
        if (!pixel())
            return false;
        done += x == 799 && y == 524;
        return true;
    }

    bool pixel()
    {
        bool dbl = canvas_w(cv) == 320;
        int ch = canvas_h(cv);
        bool de = x < 640 && (dbl ? y >= 1 && y <= 2 * ch : y < ch);
        if (dut->tb_mvideo_de != de)
            return fail("DE", dut->tb_mvideo_de, de);
        if (dut->tb_mvideo_hs != (x >= 656 && x < 752))
            return fail("HS", dut->tb_mvideo_hs, !dut->tb_mvideo_hs);
        if (dut->tb_mvideo_vs != (y == 490 || y == 491))
            return fail("VS", dut->tb_mvideo_vs, !dut->tb_mvideo_vs);
        int arx = ch == 360 || ch == 180 || ch == 400 || ch == 200 ? 16 : 4;
        int ary = ch == 360 || ch == 180 ? 9 : ch == 400 || ch == 200 ? 10 : 3;
        if (dut->tb_mvideo_arx != arx)
            return fail("ARX", dut->tb_mvideo_arx, arx);
        if (dut->tb_mvideo_ary != ary)
            return fail("ARY", dut->tb_mvideo_ary, ary);

        uint32_t rgb = ((uint32_t)dut->tb_mvideo_r << 16) |
                       ((uint32_t)dut->tb_mvideo_g << 8) | dut->tb_mvideo_b;
        uint32_t want = 0;
        if (de && live)
            want = dbl ? rgb888(bars(cv, x >> 1, (y - 1) >> 1))
                       : rgb888(bars(cv, x, y));
        if (rgb != want)
            return fail("RGB", rgb, want);
        if (de)
        {
            de_px++;
            lit += rgb != 0;
            line[x] = rgb;
            /* A row's second line is its first again. */
            if (dbl && !(y & 1) && rgb != prev_line[x])
                return fail("doubled line", rgb, prev_line[x]);
        }
        return true;
    }
};

/* Runs until the raster has ended `frames` frames, the first of them seen
 * only from its VS, calling `at` on the first pixel of line 100 of each. */
template <typename F>
static bool run(Raster &r, long frames, F at)
{
    long limit = (frames + 2) * 1600L * 525;
    int at_frame = -1;
    while (r.done < frames)
    {
        if (r.clocks > limit)
        {
            printf("mvideo: no VS in %ld clocks\n", r.clocks);
            return false;
        }
        tick();
        if (!r.clock())
            return false;
        if (r.started && r.y == 100 && r.x == 0 && at_frame != r.frames)
        {
            at_frame = (int)r.frames;
            at(r);
        }
    }
    return true;
}

static void canvas_case(int *utest_result, uint8_t cv)
{
    fresh(cv, true);
    Raster r;
    r.cv = r.next_cv = cv;
    r.live = r.next_live = true;
    ASSERT_TRUE(run(r, 3, [](Raster &) {}));
    long ch = canvas_h(cv) * (canvas_w(cv) == 320 ? 2 : 1);
    ASSERT_EQ(r.de_px, 2L * 640 * ch);
    ASSERT_GT(r.lit, r.de_px / 2);
}

UTEST(mvideo, canvas0_console)
{
    canvas_case(utest_result, 0);
}

UTEST(mvideo, canvas1_320x240)
{
    canvas_case(utest_result, 1);
}

UTEST(mvideo, canvas2_320x180)
{
    canvas_case(utest_result, 2);
}

UTEST(mvideo, canvas3_640x480)
{
    canvas_case(utest_result, 3);
}

UTEST(mvideo, canvas4_640x360)
{
    canvas_case(utest_result, 4);
}

UTEST(mvideo, canvas5_320x200)
{
    canvas_case(utest_result, 5);
}

UTEST(mvideo, canvas6_640x400)
{
    canvas_case(utest_result, 6);
}

/* The canvas changes a third of the way down a frame. The machine takes it
 * as its next frame starts and the output as its next frame does, so the
 * frame being shown keeps its canvas to the end. */
UTEST(mvideo, canvas_change_at_frame)
{
    static const uint8_t order[] = {0, 2, 6, 1, 4, 5, 0};
    fresh(order[0], true);
    Raster r;
    r.cv = r.next_cv = order[0];
    r.live = r.next_live = true;
    int step = 0;
    ASSERT_TRUE(run(r, 7, [&](Raster &rr) {
        if (++step < (int)sizeof order)
        {
            dut->canvas = order[step];
            rr.next_cv = order[step];
        }
    }));
}

/* With no vid_frame ever, sync is the same and the picture is black. */
UTEST(mvideo, no_frame_black)
{
    for (uint8_t cv : {0, 1, 6})
    {
        fresh(cv, false);
        Raster r;
        /* With no frame to set its phase, the raster's first frame is
         * the one it starts in, before it has taken a canvas. */
        r.next_cv = cv;
        ASSERT_TRUE(run(r, 3, [](Raster &) {}));
        ASSERT_GT(r.de_px, 0);
        ASSERT_EQ(r.lit, 0);
    }
}

/* When the frames stop, the frame after the last one is black and sync
 * carries on without a jump; when they come back, so does the picture. */
UTEST(mvideo, frames_stop_then_resume)
{
    fresh(1, true);
    Raster r;
    r.cv = r.next_cv = 1;
    r.live = r.next_live = true;
    int f = 0;
    ASSERT_TRUE(run(r, 6, [&](Raster &rr) {
        switch (f++)
        {
        case 1:
            dut->frame_en = 0;
            rr.next_live = false;
            break;
        case 3:
            dut->frame_en = 1;
            rr.next_live = true;
            break;
        }
    }));
    ASSERT_GT(r.lit, 0);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    int rc = utest_main(argc, argv);
    if (dut)
    {
        dut->final();
        delete dut;
    }
    return rc;
}
