/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "oracle.h"

#include "emu/emu/rom.h"
#include "emu/main.h"
#include "emu/sys/sys.h"
#include "emu/sys/vga.h"

/* Sized for the largest canvas; every smaller canvas renders at its own stride. */
static uint32_t oracle_fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

void oracle_init(void)
{
    main_init();
    vga_set_framebuffer(oracle_fb);
}

bool oracle_restart(const char *rom)
{
    main_stop();
    if (!rom_load(rom))
        return false;
    main_run();
    return true;
}

void oracle_run_frames(int count)
{
    for (int i = 0; i < count; i++)
        sys_run_frame();
}

const uint32_t *oracle_framebuffer(void)
{
    return oracle_fb;
}

void oracle_canvas_size(int *width, int *height)
{
    vga_canvas_size(width, height);
}

unsigned long oracle_frame_count(void)
{
    return sys_frame_count();
}
