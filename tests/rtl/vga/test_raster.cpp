/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tb_core.h"
#include "utest.h"

static const int TB_PX = 2;
static const int TB_H_TOTAL = 800;
static const int TB_V_TOTAL = 525;

/* The timing module has no reset input, so the frame starts at the origin
 * only because its counters power on at zero. */
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
