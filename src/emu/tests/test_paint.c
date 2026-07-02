/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 6522 VIA timer-IRQ + mouse integration, driven by paint.rp6502. Paint maps the
 * HID mouse (xreg device 0, channel 0, address 1) and arms VIA Timer 1 in
 * free-run mode for a 125 Hz IRQ; its ISR reads the mouse counters and moves the
 * on-screen pointer sprite. With no mouse motion the frame is static, so a frame
 * change after feeding host motion proves the whole chain fired: the VIA pulled
 * IRQB, the 6502 vectored through $FFFE/$FFFF to the ISR, and the ISR read the
 * mou.c XRAM mirror. This is the emulator's only IRQ-driven test.
 */

#include "emu/app/window.h"
#include "emu/hid/mou.h"
#include "emu/mon/rom.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "emu/sys/vga.h"
#include "emu/sys/via.h"
#include "utest.h"

static uint32_t fb[EMU_FB_WIDTH * EMU_FB_HEIGHT];

static uint32_t frame_crc(void)
{
    int cw, ch;
    emu_canvas_size(&cw, &ch);
    return emu_crc32(0, fb, (size_t)cw * ch * 4);
}

static void run(int n)
{
    for (int i = 0; i < n; i++)
        emu_run_frame();
}

UTEST(paint, via_irq_moves_pointer)
{
    ASSERT_TRUE(emu_rom_load(PAINT_ROM));
    emu_init();
    vga_set_framebuffer(fb);
    run(60); /* set up mode 3 + picker + pointer, map the mouse, arm VIA T1 */

    ASSERT_TRUE(mou_is_mapped()); /* xreg_ria_mouse ran */

    /* Static with no mouse input: the pointer is the only moving element and it
     * only follows the mouse via the timer ISR. */
    uint32_t still = frame_crc();
    run(20);
    ASSERT_EQ(frame_crc(), still);

    /* Move the mouse far. The pointer can only track it if the 6522 timer IRQ
     * fires and its ISR runs — so a frame change is the end-to-end proof. */
    mou_host_move(80.0f, 60.0f);
    run(20);
    ASSERT_NE(frame_crc(), still);

    ASSERT_FALSE(emu_cpu_halted);
}

UTEST_MAIN()
