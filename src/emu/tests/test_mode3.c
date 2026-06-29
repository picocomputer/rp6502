/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Integration test for VGA graphics mode 3 (the bitmap mode). mode3.rp6502
 * selects canvas 2 (320x180, a 16:9 canvas) and draws a 1bpp bitmap, scrolling
 * it. This exercises the canvas-native framebuffer (the texture/window now
 * follow the canvas size), the mode3 scanline renderer, and the xreg dispatch.
 */

#include "emu/mon/rom.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "emu/sys/vga.h"
#include "utest.h"

static uint32_t fb[EMU_FB_WIDTH * EMU_FB_HEIGHT];

static void run_frames(int n)
{
    for (int i = 0; i < n; i++)
        emu_run_frame();
}

UTEST(mode3, renders_bitmap_on_320x180_canvas)
{
    ASSERT_TRUE(emu_rom_load(MODE3_ROM));
    emu_init();
    run_frames(20);

    /* The program switched to canvas 2: the framebuffer is now its native size. */
    int cw, ch;
    emu_canvas_size(&cw, &ch);
    ASSERT_EQ(cw, 320);
    ASSERT_EQ(ch, 180);

    emu_render(fb);
    size_t total = (size_t)cw * ch, fg = 0, bg = 0;
    for (size_t i = 0; i < total; i++)
    {
        if (fb[i] == 0xFF000000u)
            bg++;
        else
            fg++;
    }
    ASSERT_GT(fg, (size_t)0); /* the 1bpp pattern drew foreground pixels */
    ASSERT_GT(bg, (size_t)0); /* on a background */
    ASSERT_FALSE(emu_cpu_halted);
}

UTEST_MAIN()
