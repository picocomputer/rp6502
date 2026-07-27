/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Verilated machine skeleton: reset behavior and the free-running scanline
 * counter that the video timing will be built on.
 */

#include "tb_core.h"
#include "utest.h"
#include "verilated.h"

/* rp6502_pkg RP6502_V_TOTAL — 640x480@60 has 525 scanlines. */
static const int TB_V_TOTAL = 525;

UTEST(rtl, reset_clears_scanline)
{
    tb_core_init();
    tb_core_reset();
    ASSERT_EQ(tb_core_scanline(), 0);
    tb_core_free();
}

UTEST(rtl, scanline_advances_one_per_clock)
{
    tb_core_init();
    tb_core_reset();
    tb_core_clocks(1);
    ASSERT_EQ(tb_core_scanline(), 1);
    tb_core_clocks(1);
    ASSERT_EQ(tb_core_scanline(), 2);
    tb_core_free();
}

UTEST(rtl, scanline_wraps_at_frame_end)
{
    tb_core_init();
    tb_core_reset();
    tb_core_clocks(TB_V_TOTAL - 1);
    ASSERT_EQ(tb_core_scanline(), TB_V_TOTAL - 1);
    tb_core_clocks(1);
    ASSERT_EQ(tb_core_scanline(), 0);
    tb_core_free();
}

UTEST(rtl, scanline_stays_in_range_over_frames)
{
    tb_core_init();
    tb_core_reset();
    for (int i = 0; i < TB_V_TOTAL * 3; i++)
    {
        ASSERT_LT(tb_core_scanline(), TB_V_TOTAL);
        tb_core_clocks(1);
    }
    tb_core_free();
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    return utest_main(argc, argv);
}
