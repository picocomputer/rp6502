/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Integration test for VGA graphics mode 4 (sprites) — the affine variant.
 * affine.rp6502 uploads a 128x128 photographic sprite and programs 10 affine
 * sprites (attribute 1) on canvas 1 (320x240), scaling each via its 2x3
 * transform matrix. This exercises mode4's affine texture mapping, which on the
 * Pico runs on the hardware interpolator and off-device on vga/modes/mode4.c's
 * software interpolator fallback (guarded by PICO_ON_DEVICE).
 */

#include "emu/mon/rom.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "emu/sys/vga.h"
#include "utest.h"
#include <string.h>

static uint32_t fb[EMU_FB_WIDTH * EMU_FB_HEIGHT];
static uint8_t seen[1 << 16];

static void run_frames(int n)
{
    for (int i = 0; i < n; i++)
        emu_run_frame();
}

/* A photographic sprite mapped through an affine transform yields a large,
 * many-colored region — proof the texture sampler ran, not a flat fill. */
UTEST(mode4, affine_sprites_texture_map)
{
    ASSERT_TRUE(emu_rom_load(MODE4_ROM));
    emu_init();
    run_frames(60);

    int cw, ch;
    emu_canvas_size(&cw, &ch);
    ASSERT_EQ(cw, 320);
    ASSERT_EQ(ch, 240);

    emu_render(fb);
    memset(seen, 0, sizeof seen);
    size_t total = (size_t)cw * ch, nonblack = 0, distinct = 0;
    for (size_t i = 0; i < total; i++)
    {
        if (fb[i] == 0xFF000000u)
            continue;
        nonblack++;
        uint32_t r = fb[i] & 0xFF, g = (fb[i] >> 8) & 0xFF, b = (fb[i] >> 16) & 0xFF;
        uint16_t key = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        if (!seen[key])
        {
            seen[key] = 1;
            distinct++;
        }
    }

    ASSERT_GT(nonblack, (size_t)2000); /* the scaled sprites drew */
    ASSERT_GT(distinct, (size_t)30);   /* a sampled photo, not a solid block */
    ASSERT_FALSE(emu_cpu_halted);      /* the demo loops forever */
}

UTEST_MAIN()
