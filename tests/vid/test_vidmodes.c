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

#include <stdio.h>
#include <string.h>

static uint32_t fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];
static uint32_t settled[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

static void run_frames(int n)
{
    for (int i = 0; i < n; i++)
        sys_run_frame();
}

/* The corpus is generated and cannot be enumerated from here, so it says its
 * own shape: one line of name, width and height per ROM. Carrying the geometry
 * at each call site meant forty-seven pairs of numbers that no one could check
 * against the generator, and one of them was wrong. */
static bool corpus_size(const char *name, int *width, int *height)
{
    FILE *f = fopen(ROMS_DIR "/manifest.txt", "r");
    if (!f)
        return false;
    char n[128];
    int w, h;
    bool found = false;
    while (fscanf(f, "%127s %d %d", n, &w, &h) == 3)
        if (!strcmp(n, name))
        {
            *width = w, *height = h, found = true;
            break;
        }
    fclose(f);
    return found;
}

static void run_case(int *utest_result, const char *name)
{
    int width, height;
    ASSERT_TRUE(corpus_size(name, &width, &height));

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
    run_case(utest_result, "mode3_8bpp");
}

UTEST(vidmodes, mode3_1bpp)
{
    run_case(utest_result, "mode3_1bpp");
}

UTEST(vidmodes, mode3_4bppr)
{
    run_case(utest_result, "mode3_4bppr");
}

UTEST(vidmodes, mode3_16bpp)
{
    run_case(utest_result, "mode3_16bpp");
}

UTEST(vidmodes, mode1_1bpp8x8)
{
    run_case(utest_result, "mode1_1bpp8x8");
}

UTEST(vidmodes, mode1_4bpp8x16)
{
    run_case(utest_result, "mode1_4bpp8x16");
}

UTEST(vidmodes, mode1_4bppr8x8)
{
    run_case(utest_result, "mode1_4bppr8x8");
}

UTEST(vidmodes, mode1_8bpp8x8)
{
    run_case(utest_result, "mode1_8bpp8x8");
}

UTEST(vidmodes, mode1_16bpp8x16)
{
    run_case(utest_result, "mode1_16bpp8x16");
}

UTEST(vidmodes, mode2_1bpp8)
{
    run_case(utest_result, "mode2_1bpp8");
}

UTEST(vidmodes, mode2_2bpp16)
{
    run_case(utest_result, "mode2_2bpp16");
}

UTEST(vidmodes, mode2_4bpp8trim)
{
    run_case(utest_result, "mode2_4bpp8trim");
}

UTEST(vidmodes, mode2_8bpp16wrap)
{
    run_case(utest_result, "mode2_8bpp16wrap");
}

UTEST(vidmodes, mode2_composite)
{
    run_case(utest_result, "mode2_composite");
}

UTEST(vidmodes, mode3_2bpp)
{
    run_case(utest_result, "mode3_2bpp");
}

UTEST(vidmodes, mode3_4bpp)
{
    run_case(utest_result, "mode3_4bpp");
}

UTEST(vidmodes, mode3_1bppr)
{
    run_case(utest_result, "mode3_1bppr");
}

UTEST(vidmodes, mode3_2bppr)
{
    run_case(utest_result, "mode3_2bppr");
}

UTEST(vidmodes, mode3_wrap)
{
    run_case(utest_result, "mode3_wrap");
}

UTEST(vidmodes, mode1_wrap)
{
    run_case(utest_result, "mode1_wrap");
}

UTEST(vidmodes, mode2_16trim)
{
    run_case(utest_result, "mode2_16trim");
}

UTEST(vidmodes, mode2_trimx)
{
    run_case(utest_result, "mode2_trimx");
}

UTEST(vidmodes, mode2_trimx8)
{
    run_case(utest_result, "mode2_trimx8");
}

UTEST(vidmodes, mode2_trimy)
{
    run_case(utest_result, "mode2_trimy");
}

UTEST(vidmodes, mode5_8x8)
{
    run_case(utest_result, "mode5_8x8");
}

UTEST(vidmodes, mode5_16x16)
{
    run_case(utest_result, "mode5_16x16");
}

UTEST(vidmodes, mode5_32x32)
{
    run_case(utest_result, "mode5_32x32");
}

UTEST(vidmodes, mode5_64x64)
{
    run_case(utest_result, "mode5_64x64");
}

UTEST(vidmodes, mode4_8)
{
    run_case(utest_result, "mode4_8");
}

UTEST(vidmodes, mode4_meta16)
{
    run_case(utest_result, "mode4_meta16");
}

UTEST(vidmodes, mode4_32)
{
    run_case(utest_result, "mode4_32");
}

UTEST(vidmodes, mode4_64)
{
    run_case(utest_result, "mode4_64");
}

UTEST(vidmodes, mode4a_id)
{
    run_case(utest_result, "mode4a_id");
}

UTEST(vidmodes, mode4a_rot)
{
    run_case(utest_result, "mode4a_rot");
}

UTEST(vidmodes, mode4a_clip)
{
    run_case(utest_result, "mode4a_clip");
}

UTEST(vidmodes, sprite_stress)
{
    run_case(utest_result, "sprite_stress");
}

UTEST(vidmodes, mode5_1bpp128)
{
    run_case(utest_result, "mode5_1bpp128");
}

UTEST(vidmodes, mode5_4bpp256)
{
    run_case(utest_result, "mode5_4bpp256");
}

UTEST(vidmodes, mode4_sizes)
{
    run_case(utest_result, "mode4_sizes");
}

UTEST(vidmodes, mode4a_sizes)
{
    run_case(utest_result, "mode4a_sizes");
}

UTEST(vidmodes, sprite_overrun_rom)
{
    run_case(utest_result, "sprite_overrun");
}

UTEST(vidmodes, fill_heavy640)
{
    run_case(utest_result, "fill_heavy640");
}

UTEST(vidmodes, mode0_overlay)
{
    run_case(utest_result, "mode0_overlay");
}

UTEST(vidmodes, mode0_win360)
{
    run_case(utest_result, "mode0_win360");
}

UTEST(vidmodes, mode0_win240)
{
    run_case(utest_result, "mode0_win240");
}

UTEST(vidmodes, mode0_win180)
{
    run_case(utest_result, "mode0_win180");
}

UTEST(vidmodes, mode0_return)
{
    run_case(utest_result, "mode0_return");
}

UTEST_MAIN_EMU()
