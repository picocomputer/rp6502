/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Integration test for VGA graphics mode 1 (the tile/text mode). mode1.rp6502
 * programs a Commodore-64 BASIC screen, scrolls it on vsync, and exits on a
 * key press read from the xreg keyboard bitmap. This exercises the xreg mode
 * dispatch, the mode1 scanline renderer, and the HID keyboard XRAM bitmap.
 */

#include "emu/hid/kbd.h"
#include "emu/mon/rom.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "term/color.h"
#include "utest.h"

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

/* The screen is a mostly-uniform C64 background (palette index 12) with sparse
 * text — proves the mode1 renderer drew the tilemap, not the terminal. */
UTEST(mode1, renders_c64_screen)
{
    ASSERT_TRUE(emu_rom_load(MODE1_ROM));
    emu_init();
    run_frames(10);
    emu_render(fb);

    uint32_t bg = to_rgba(color_256[12]);
    size_t total = EMU_FB_WIDTH * EMU_FB_HEIGHT, n_bg = 0, n_other = 0;
    for (size_t i = 0; i < total; i++)
    {
        if (fb[i] == bg)
            n_bg++;
        else
            n_other++;
    }

    ASSERT_GT(n_bg, total / 2); /* C64 background dominates */
    ASSERT_GT(n_other, (size_t)0); /* text/cursor present */
    ASSERT_FALSE(emu_cpu_halted); /* still scrolling */
}

/* The scroll loop polls the keyboard bitmap: it runs while no key is down and
 * exits once a key is pressed and released. Inject that via the HID bitmap. */
UTEST(mode1, keyboard_press_exits)
{
    ASSERT_TRUE(emu_rom_load(MODE1_ROM));
    emu_init();
    run_frames(20);
    ASSERT_FALSE(emu_cpu_halted); /* scrolling, no key down */

    kbd_hid_set(0x2C, true); /* press space -> "any key" bit clears */
    run_frames(5);
    ASSERT_FALSE(emu_cpu_halted); /* waiting for the release */

    kbd_hid_set(0x2C, false); /* release -> program exits the scroll loop */
    run_frames(10);
    ASSERT_TRUE(emu_cpu_halted);
}

UTEST_MAIN()
