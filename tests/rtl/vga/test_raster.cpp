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

/* The screen trails the beam by a line: what the beam draws on line N
 * goes out on line N + 1, so the console canvas's 480 rows go out on the
 * beam's lines 1 to 480, a pixel on each pixel's last clock. */
UTEST(rtl, the_screen_trails_the_beam_by_a_line)
{
    tb_core_init();
    for (int v = 0; v < TB_V_TOTAL; v++)
        for (int h = 0; h < TB_H_TOTAL; h++)
            for (int tick = 0; tick < TB_PX; tick++)
            {
                ASSERT_EQ(tb_core_h(), h);
                ASSERT_EQ(tb_core_scanline(), v);
                bool de = h < 640 && v >= 1 && v <= 480 && tick == TB_PX - 1;
                ASSERT_EQ(tb_core_de(), de);
                tb_core_clocks(1);
            }
    ASSERT_EQ(tb_core_scanline(), 0);
    ASSERT_EQ(tb_core_h(), 0);
    tb_core_free();
}

/* The VSYNC comes once a frame, as the beam starts the last line it draws:
 * line 479 of the console canvas, as the C renderer has it. */
UTEST(rtl, vsync_comes_as_the_last_line_drawn_starts)
{
    tb_core_init();
    int pulses = 0;
    for (int v = 0; v < TB_V_TOTAL; v++)
        for (int h = 0; h < TB_H_TOTAL; h++)
            for (int tick = 0; tick < TB_PX; tick++)
            {
                bool at = v == 479 && h == 0 && tick == 0;
                ASSERT_EQ(tb_core_vsync(), at);
                pulses += tb_core_vsync();
                tb_core_clocks(1);
            }
    ASSERT_EQ(pulses, 1);
    tb_core_free();
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    tb_core_args(argc, argv);
    return utest_main(argc, argv);
}
