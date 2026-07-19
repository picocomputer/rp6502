/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Integration test for VGA graphics mode 2 (the tiled bitmap mode). mode2.rp6502
 * selects canvas 1 (320x240), fills a 40x30 tilemap with 1bpp 8x8 tiles and
 * scrolls it; on a key press it reprograms with 16x16 tiles and scrolls again.
 * This exercises the xreg mode dispatch, the mode2 scanline renderer (tile fetch
 * + palette), and the HID keyboard XRAM bitmap.
 */

#include "emu/hid/kbd.h"
#include "emu/sys/cpu.h"
#include "emu/sys/mem.h"
#include "emu/sys/vga.h"
#include "vga/term/color.h"
#include "emu_boot.h"

static uint32_t fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

/* Same RGB555(+alpha) -> RGBA8 conversion vga.c's rgb555_to_rgba8 does. */
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
        main_run_frame();
}

/* Both tile glyphs are diagonal 1bpp patterns, so the tilemap mixes the two
 * palette colors regardless of which tile each cell holds — proving the mode2
 * tile renderer drew onto its 320x240 canvas (not the terminal). */
UTEST(mode2, renders_tilemap_on_320x240_canvas)
{
    ASSERT_TRUE(emu_restart(MODE2_ROM));
    vga_set_framebuffer(fb);
    run_frames(20);

    int cw, ch;
    vga_canvas_size(&cw, &ch);
    ASSERT_EQ(cw, 320);
    ASSERT_EQ(ch, 240);

    uint32_t fg = to_rgba(color_2[1]);
    size_t total = (size_t)cw * ch, n_fg = 0, n_bg = 0;
    for (size_t i = 0; i < total; i++)
    {
        if (fb[i] == fg)
            n_fg++;
        else
            n_bg++;
    }
    ASSERT_GT(n_fg, (size_t)0);   /* tile foreground pixels drew */
    ASSERT_GT(n_bg, (size_t)0);   /* on a background */
    ASSERT_FALSE(cpu_halted()); /* still scrolling */
}

/* The program runs two scroll loops (8x8 then 16x16 tiles); each polls the
 * keyboard bitmap and exits on a key press+release. Two cycles drive it to
 * completion. */
UTEST(mode2, keyboard_presses_exit)
{
    ASSERT_TRUE(emu_restart(MODE2_ROM));
    vga_set_framebuffer(fb);
    run_frames(20);
    ASSERT_FALSE(cpu_halted()); /* scrolling, no key down */

    /* First scroll -> exit -> reprogram to 16x16 tiles, scroll again. */
    kbd_hid_set(0x2C, true); /* press space */
    run_frames(5);
    kbd_hid_set(0x2C, false); /* release */
    run_frames(10);
    ASSERT_FALSE(cpu_halted()); /* now in the second scroll loop */

    /* Second scroll -> exit -> program prints and exits. */
    kbd_hid_set(0x2C, true);
    run_frames(5);
    kbd_hid_set(0x2C, false);
    run_frames(10);
    ASSERT_TRUE(cpu_halted());
}

/* A program stop resets the VGA to the console canvas (firmware vga_stop ->
 * vga_task DISPLAY reset), so the next program starts on the console instead of
 * the previous program's graphics frame. Mirrors what a dropped ROM does: it
 * calls main_stop, and the following frame's vga_task performs the reset. */
UTEST(mode2, stop_resets_canvas_to_console)
{
    ASSERT_TRUE(emu_restart(MODE2_ROM));
    vga_set_framebuffer(fb);
    run_frames(20);
    ASSERT_EQ(vga_get_canvas(), vga_canvas_320_240); /* gfx canvas active */

    main_stop();      /* the outgoing program stops (as on a ROM swap) */
    main_run_frame(); /* vga_task performs the armed console reset */
    ASSERT_EQ(vga_get_canvas(), vga_canvas_console);
}

UTEST_MAIN_EMU()
