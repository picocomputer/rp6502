/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Boots every mini-ROM in the tests/roms video-mode corpus: each one
 * programs a canvas and one mode 1 or mode 3 plane, then stops. Asserts
 * the canvas took, the plane drew something, and the picture settled.
 * The FPGA suite runs these same files against the RTL machine and
 * demands pixel equality with this renderer, so together the two suites
 * pin both implementations to one corpus.
 */

#include "emu/sys/vga.h"
#include "emu_boot.h"

#include <string.h>

static uint32_t fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];
static uint32_t settled[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

static void run_frames(int n)
{
    for (int i = 0; i < n; i++)
        sys_run_frame();
}

static void run_case(int *utest_result, const char *name, int width, int height)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.rp6502", ROMS_DIR, name);
    ASSERT_TRUE(emu_restart(path));
    vga_set_framebuffer(fb);
    run_frames(20);

    int cw, ch;
    vga_canvas_size(&cw, &ch);
    ASSERT_EQ(cw, width);
    ASSERT_EQ(ch, height);

    size_t total = (size_t)cw * ch, n_black = 0;
    for (size_t i = 0; i < total; i++)
        if (fb[i] == 0xFF000000u)
            n_black++;
    ASSERT_LT(n_black, total); /* the plane drew */
    /* A wraparound window tiles the whole canvas; every other window
     * leaves black around itself. */
    if (!strstr(name, "wrap"))
        ASSERT_GT(n_black, (size_t)0);

    memcpy(settled, fb, total * sizeof(uint32_t));
    run_frames(5);
    ASSERT_EQ(memcmp(settled, fb, total * sizeof(uint32_t)), 0);
}

UTEST(vidmodes, mode3_8bpp)
{
    run_case(utest_result, "mode3_8bpp", 640, 480);
}

UTEST(vidmodes, mode3_1bpp)
{
    run_case(utest_result, "mode3_1bpp", 320, 240);
}

UTEST(vidmodes, mode3_4bppr)
{
    run_case(utest_result, "mode3_4bppr", 320, 180);
}

UTEST(vidmodes, mode3_16bpp)
{
    run_case(utest_result, "mode3_16bpp", 640, 360);
}

UTEST(vidmodes, mode1_1bpp8x8)
{
    run_case(utest_result, "mode1_1bpp8x8", 640, 480);
}

UTEST(vidmodes, mode1_4bpp8x16)
{
    run_case(utest_result, "mode1_4bpp8x16", 320, 240);
}

UTEST(vidmodes, mode1_4bppr8x8)
{
    run_case(utest_result, "mode1_4bppr8x8", 320, 180);
}

UTEST(vidmodes, mode1_8bpp8x8)
{
    run_case(utest_result, "mode1_8bpp8x8", 320, 240);
}

UTEST(vidmodes, mode1_16bpp8x16)
{
    run_case(utest_result, "mode1_16bpp8x16", 640, 360);
}

UTEST(vidmodes, mode2_1bpp8)
{
    run_case(utest_result, "mode2_1bpp8", 640, 480);
}

UTEST(vidmodes, mode2_2bpp16)
{
    run_case(utest_result, "mode2_2bpp16", 320, 240);
}

UTEST(vidmodes, mode2_4bpp8trim)
{
    run_case(utest_result, "mode2_4bpp8trim", 320, 240);
}

UTEST(vidmodes, mode2_8bpp16wrap)
{
    run_case(utest_result, "mode2_8bpp16wrap", 320, 180);
}

UTEST(vidmodes, mode2_composite)
{
    run_case(utest_result, "mode2_composite", 320, 240);
}

UTEST(vidmodes, mode3_2bpp)
{
    run_case(utest_result, "mode3_2bpp", 320, 240);
}

UTEST(vidmodes, mode3_4bpp)
{
    run_case(utest_result, "mode3_4bpp", 640, 480);
}

UTEST(vidmodes, mode3_1bppr)
{
    run_case(utest_result, "mode3_1bppr", 320, 180);
}

UTEST(vidmodes, mode3_2bppr)
{
    run_case(utest_result, "mode3_2bppr", 640, 360);
}

UTEST(vidmodes, mode3_wrap)
{
    run_case(utest_result, "mode3_wrap", 320, 240);
}

UTEST(vidmodes, mode1_wrap)
{
    run_case(utest_result, "mode1_wrap", 320, 240);
}

UTEST(vidmodes, mode2_16trim)
{
    run_case(utest_result, "mode2_16trim", 320, 240);
}

UTEST(vidmodes, mode2_trimx)
{
    run_case(utest_result, "mode2_trimx", 640, 480);
}

UTEST(vidmodes, mode2_trimx8)
{
    run_case(utest_result, "mode2_trimx8", 320, 180);
}

UTEST(vidmodes, mode2_trimy)
{
    run_case(utest_result, "mode2_trimy", 320, 180);
}

UTEST_MAIN_EMU()
