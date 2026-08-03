/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 640x480@60 raster at the two-clock render tick: 1,600 clocks a
 * line, 525 lines a frame, syncs active-low in their exact windows — h
 * 656..751 of every line, v lines 490..491 — and de strobing once per
 * visible pixel, on its final tick.
 */

#include "tb_core.h"
#include "utest.h"

static const int TB_PX = 2;
static const int TB_H_TOTAL = 800;
static const int TB_V_TOTAL = 525;

/* The beam takes no reset — it is not the 6502 or the 6522 — so where a
 * frame starts is decided at power-on and nowhere else. Every frame
 * comparison in the suite rests on that being the origin. */
UTEST(rtl, power_on_starts_at_the_origin)
{
    tb_core_init();
    ASSERT_EQ(tb_core_scanline(), 0);
    ASSERT_EQ(tb_core_h(), 0);
    tb_core_free();
}

UTEST(rtl, scanline_advances_once_per_line)
{
    tb_core_init();
    tb_core_clocks(TB_H_TOTAL * TB_PX - 1);
    ASSERT_EQ(tb_core_scanline(), 0);
    tb_core_clocks(1);
    ASSERT_EQ(tb_core_scanline(), 1);
    tb_core_clocks(TB_H_TOTAL * TB_PX);
    ASSERT_EQ(tb_core_scanline(), 2);
    tb_core_free();
}

UTEST(rtl, frame_wraps_after_525_lines)
{
    tb_core_init();
    tb_core_clocks((TB_V_TOTAL - 1) * TB_H_TOTAL * TB_PX);
    ASSERT_EQ(tb_core_scanline(), TB_V_TOTAL - 1);
    tb_core_clocks(TB_H_TOTAL * TB_PX);
    ASSERT_EQ(tb_core_scanline(), 0);
    tb_core_free();
}

UTEST(rtl, sync_and_de_windows_are_exact)
{
    tb_core_init();
    /* One full frame, every clock checked against the window math. */
    for (int v = 0; v < TB_V_TOTAL; v++)
        for (int h = 0; h < TB_H_TOTAL; h++)
            for (int tick = 0; tick < TB_PX; tick++)
            {
                ASSERT_EQ(tb_core_h(), h);
                ASSERT_EQ(tb_core_scanline(), v);
                bool hs_low = h >= 656 && h <= 751;
                bool vs_low = v >= 490 && v <= 491;
                bool de = h < 640 && v < 480 && tick == TB_PX - 1;
                ASSERT_EQ(tb_core_hsync(), !hs_low);
                ASSERT_EQ(tb_core_vsync(), !vs_low);
                ASSERT_EQ(tb_core_de(), de);
                tb_core_clocks(1);
            }
    /* Exactly 1,680,000 clocks later the frame starts over. */
    ASSERT_EQ(tb_core_scanline(), 0);
    ASSERT_EQ(tb_core_h(), 0);
    tb_core_free();
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    tb_core_args(argc, argv);
    return utest_main(argc, argv);
}
