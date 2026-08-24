/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine under test, when it is emu_core.
 *
 * Everything here is the emulator's own lifecycle, which emu_boot.h already
 * describes: init once for the binary, load/run per program, a frame at a
 * time from there.
 */

#include "mut.h"

#include "core/emu/sys/com.h"
#include "core/emu/sys/mem.h"
#include "core/emu/sys/vga.h"
#include "emu_boot.h"

#include <string.h>

static uint32_t mut_fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

/* Frames between a program stopping and its picture being worth reading.
 * The corpus ROMs program a canvas and a plane and stop, and the console
 * canvas is reset a frame late, so this is comfortably past both. */
#define MUT_SETTLE_FRAMES 20

void mut_init(int argc, const char *const argv[])
{
    (void)argc;
    (void)argv;
    /* This machine's RAM and XRAM come up random, because a 6502's SRAM does
     * and a program reading a byte it never wrote should fail here rather
     * than only on hardware. The fabric's are block RAM and come up zeroed.
     * A suite written to both has to be given one answer, and it is the one
     * that can be reproduced: an expectation written down against a random
     * fill would be a different number every run. */
    mem_set_fill(false, 0, 0);
    main_init();
}

void mut_free(void)
{
}

/* The terminal's single sink. Everything the machine sends a terminal comes
 * through com_tx_write, the OS's own bytes among them. */
static char mut_tap[65536];
static size_t mut_tap_len;

static void mut_tap_write(const char *buf, int len)
{
    for (int i = 0; i < len && mut_tap_len < sizeof mut_tap; i++)
        mut_tap[mut_tap_len++] = buf[i];
}

void mut_console_start(void)
{
    mut_tap_len = 0;
    com_set_tx_tap(mut_tap_write);
}

const char *mut_console(size_t *len)
{
    *len = mut_tap_len;
    return mut_tap;
}

bool mut_boot(const char *rom)
{
    if (!emu_restart(rom))
        return false;
    vga_set_framebuffer(mut_fb);
    for (int i = 0; i < MUT_SETTLE_FRAMES; i++)
        sys_run_frame();
    return true;
}

const uint32_t *mut_frame(int w, int h)
{
    (void)w;
    (void)h;
    sys_run_frame();
    return mut_fb;
}

/* No beam here. The renderer draws a scanline when asked and takes as long as
 * it takes; there is no deadline it could miss. */
mut_budget_t mut_measure(const char *name)
{
    (void)name;
    return MUT_BUDGET_NONE;
}
