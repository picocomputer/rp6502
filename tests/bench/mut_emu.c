/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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

#define MUT_SETTLE_FRAMES 20

void mut_init(int argc, const char *const argv[])
{
    (void)argc;
    (void)argv;
    sram_set_fill(false, 0, 0);
    xram_set_fill(false, 0, 0);
    sys_init();
}

void mut_free(void)
{
}

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

mut_budget_t mut_measure(const char *name)
{
    (void)name;
    return MUT_BUDGET_NONE;
}
