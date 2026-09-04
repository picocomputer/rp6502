/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine under test, when it is emu_core.
 *
 * Everything here is the emulator's own bring-up, which emu_boot.h already
 * describes: init once for the binary, load/run per program, a frame at a
 * time from there.
 */

#include "core/sys/proc.h"
#include "core/sys/sys.h"
#include "mut.h"

#include "core/com/com.h"
#include "core/wdc/sram.h"
#include "core/sys/xram.h"
#include "core/vga/vga_emu.h"
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
    sram_set_fill(false, 0, 0);
    xram_set_fill(false, 0, 0);
    sys_init();
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
    /* A boot is a fresh machine, which is emu_restart plus the refill: the
     * other machine gets one by construction and a suite written to both has
     * to be able to say what XRAM held before its program ran. The fills come
     * before the loader in the drivers, so its bytes still land on top. */
    if (!proc_boot(rom, 0, NULL, PROC_REFILL))
        return false;
    sys_commit();
    vga_set_framebuffer(mut_fb);
    emu_frames((int)MUT_SETTLE_FRAMES);
    return true;
}

void mut_xram(uint32_t addr, uint8_t *dst, size_t len)
{
    memcpy(dst, (const uint8_t *)&xram[addr], len);
}

const uint32_t *mut_frame(int w, int h)
{
    (void)w;
    (void)h;
    emu_frames(1);
    return mut_fb;
}

/* No beam here. The renderer draws a scanline when asked and takes as long as
 * it takes; there is no deadline it could miss. */
mut_budget_t mut_measure(const char *name)
{
    (void)name;
    return MUT_BUDGET_NONE;
}
