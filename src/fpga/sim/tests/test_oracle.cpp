/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The reference oracle links and runs: emu_core boots headless, advances frames
 * and renders a program's output. Every RTL comparison test depends on this.
 */

#include "oracle.h"
#include "utest.h"

UTEST(oracle, boots_to_console_canvas)
{
    int width = 0, height = 0;
    oracle_canvas_size(&width, &height);
    ASSERT_EQ(width, 640);
    ASSERT_EQ(height, 480);
}

UTEST(oracle, frames_advance)
{
    unsigned long before = oracle_frame_count();
    oracle_run_frames(3);
    ASSERT_EQ(oracle_frame_count(), before + 3);
}

UTEST(oracle, runs_a_rom_and_renders_pixels)
{
    ASSERT_TRUE(oracle_restart(ADVENTURE_ROM));
    oracle_run_frames(30);

    int width = 0, height = 0;
    oracle_canvas_size(&width, &height);
    const uint32_t *fb = oracle_framebuffer();

    size_t lit = 0;
    for (size_t i = 0, n = (size_t)width * height; i < n; i++)
        if ((fb[i] & 0x00FFFFFFu) != 0)
            lit++;
    ASSERT_GT(lit, (size_t)0);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    oracle_init();
    return utest_main(argc, argv);
}
