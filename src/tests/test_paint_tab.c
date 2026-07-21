/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Absolute-pointer ("tablet") integration, driven by paint_tab.rp6502. Paint
 * maps the tablet (xreg device 0, channel 0, address 3) and, in its main loop,
 * decodes contact 0's absolute X/Y from the unary window encoding and moves the
 * on-screen pointer sprite. Feeding an absolute host position and seeing the
 * frame change proves the whole chain: tab.c window-encoded XRAM -> the ROM's
 * first-non-zero decode -> the sprite. No capture / no VIA IRQ, unlike the mouse.
 */

#include "emu/hid/tab.h"
#include "emu/sys/cpu.h"
#include "emu/sys/mem.h"
#include "emu/sys/vga.h"
#include "emu_boot.h"

static uint32_t fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

static uint32_t frame_crc(void)
{
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    return mem_crc32(0, fb, (size_t)cw * ch * 4);
}

static void run(int n)
{
    for (int i = 0; i < n; i++)
        sys_run_frame();
}

UTEST(paint_tab, absolute_pointer_moves)
{
    ASSERT_TRUE(emu_restart(PAINT_TAB_ROM));
    vga_set_framebuffer(fb);
    run(60); /* set up mode 3 + picker + pointer, map the tablet */

    ASSERT_TRUE(tab_is_mapped()); /* xreg_ria_tablet ran */

    /* A touch reports host_cursor=0, so the ROM draws its own sprite (and, tip
     * down, paints). Held still, the frame is static. */
    tab_point_t p = {60, 60};
    tab_host_touch(&p, 1);
    run(10);
    uint32_t still = frame_crc();
    run(20);
    ASSERT_EQ(frame_crc(), still);

    /* Move it far. Crossing the 254/255 X window edge exercises the window
     * decode; the sprite/stroke can only follow if the ROM decoded the new
     * absolute position, so a frame change is the end-to-end proof. */
    p.x = 280;
    p.y = 200;
    tab_host_touch(&p, 1);
    run(20);
    ASSERT_NE(frame_crc(), still);

    ASSERT_FALSE(cpu_halted());
}

UTEST_MAIN_EMU()
