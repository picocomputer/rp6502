/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Integration test for VGA graphics mode 5 (sprites) and the multi-plane
 * compositor. mode5.rp6502 (an "endless runner" demo) generates an asteroid
 * bitmap on the 6502, then programs canvas 1 (320x240) with: a terminal on
 * plane 1, star sprites on plane 0 (behind, showing through the terminal's
 * transparent background), runner sprites over the terminal on plane 1, and a
 * large 128x128 asteroid sprite on plane 2 over everything. This exercises
 * vga_prog_sprite, the per-scanline plane table, and alpha compositing.
 */

#include "emu/mon/rom.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "emu/sys/vga.h"
#include "utest.h"

#define RGB5(r, g, b) (((uint16_t)(b) << 11) | ((uint16_t)(g) << 6) | (uint16_t)(r))

static uint32_t fb[EMU_FB_WIDTH * EMU_FB_HEIGHT];

/* Same RGB555(+alpha) -> RGBA8 conversion vga.c builds its LUT with. */
static uint32_t to_rgba(uint16_t p)
{
    uint32_t r5 = p & 0x1F, g5 = (p >> 6) & 0x1F, b5 = (p >> 11) & 0x1F;
    uint32_t r = (r5 << 3) | (r5 >> 2);
    uint32_t g = (g5 << 3) | (g5 >> 2);
    uint32_t b = (b5 << 3) | (b5 >> 2);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

static void run_frames(int n)
{
    for (int i = 0; i < n; i++)
        emu_run_frame();
}

UTEST(mode5, sprites_compose_across_planes)
{
    ASSERT_TRUE(emu_rom_load(MODE5_ROM));
    emu_init();
    /* The 128x128 asteroid is generated pixel-by-pixel on the 6502 before the
     * VGA is programmed; the canvas switch below is the completion signal. */
    run_frames(480);

    int cw, ch;
    emu_canvas_size(&cw, &ch);
    ASSERT_EQ(cw, 320); /* program switched to canvas 1 -> setup finished */
    ASSERT_EQ(ch, 240);

    emu_render(fb);

    /* The asteroid's two grey body shades are produced only by the plane-2
     * 128x128 sprite — proof a large sprite rendered. */
    uint32_t ast_light = to_rgba(RGB5(28, 28, 28));
    uint32_t ast_mid = to_rgba(RGB5(18, 18, 18));
    /* A few of the eight star colors (plane 0). Stars sit behind the terminal
     * plane, so seeing them proves alpha compositing across planes. */
    uint32_t star_r = to_rgba(RGB5(31, 0, 0));
    uint32_t star_g = to_rgba(RGB5(0, 31, 0));
    uint32_t star_w = to_rgba(RGB5(31, 31, 31));

    size_t total = (size_t)cw * ch, nonblack = 0, asteroid = 0, stars = 0;
    for (size_t i = 0; i < total; i++)
    {
        if (fb[i] != 0xFF000000u)
            nonblack++;
        if (fb[i] == ast_light || fb[i] == ast_mid)
            asteroid++;
        if (fb[i] == star_r || fb[i] == star_g || fb[i] == star_w)
            stars++;
    }

    ASSERT_GT(nonblack, (size_t)1500); /* stars + runners + asteroid + text */
    ASSERT_GT(asteroid, (size_t)500);  /* the plane-2 sprite drew */
    ASSERT_GT(stars, (size_t)0);       /* plane-0 stars show through plane 1 */
    ASSERT_FALSE(emu_cpu_halted);      /* the demo loops forever */
}

UTEST_MAIN()
