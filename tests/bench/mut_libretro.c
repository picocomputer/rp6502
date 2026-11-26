/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/tablet.h"
#include "mut.h"
#include "retro_fe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUT_SETTLE_FRAMES 20

static uint32_t mut_fb[640 * 480];

void mut_init(int argc, const char *const argv[])
{
    (void)argc;
    (void)argv;
    fe_open();
    for (int i = 0; i < fe.option_count; i++)
        if (!strcmp(fe.option_key[i], "rp6502_mem_fill"))
            fe.option_value[i] = "00";
}

void mut_free(void)
{
    fe_close();
}

bool mut_boot(const char *rom)
{
    if (fe.frame_w)
        fe.unload_game();
    if (!fe_load(rom))
        return false;
    fe_run(MUT_SETTLE_FRAMES);
    return true;
}

void mut_xram(uint32_t addr, uint8_t *dst, size_t len)
{
    const uint8_t *xram = fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    if (!xram || fe.get_memory_size(RETRO_MEMORY_VIDEO_RAM) < addr + len)
    {
        memset(dst, 0, len);
        return;
    }
    memcpy(dst, xram + addr, len);
}

const uint32_t *mut_frame(int w, int h)
{
    (void)w;
    (void)h;
    fe.run();
    size_t n = (size_t)fe.frame_w * fe.frame_h;
    if (n > sizeof mut_fb / sizeof *mut_fb)
        n = sizeof mut_fb / sizeof *mut_fb;
    for (size_t i = 0; i < n; i++)
    {
        uint32_t v = fe.frame_copy[i];
        mut_fb[i] = 0xFF000000u | (v & 0x0000FF00u) |
                    ((v & 0x000000FFu) << 16) | ((v >> 16) & 0xFFu);
    }
    return mut_fb;
}

void mut_console_start(void)
{
    fprintf(stderr, "mut_console: the libretro ABI carries no terminal; "
                    "a suite whose claim is what the machine said belongs on "
                    "the machine, not on the artifact\n");
    exit(1);
}

const char *mut_console(size_t *len)
{
    mut_console_start();
    *len = 0;
    return "";
}

/* The frontend's absolute pointer at the pixel, the inverse of the core's
 * scale (src/host/libretro/input.c), rounded up so the core lands on it. */
static int16_t mut_axis(int p, int extent)
{
    return (int16_t)((p * 0xFFFE + extent - 2) / (extent - 1) - 0x7FFF);
}

bool mut_has_pointer(void)
{
    return true;
}

/* A mouse's buttons, the pointer where it is: a mouse that has moved, so
 * the core takes the pointer's place as the mouse's, and no finger. */
void mut_pointer(int x, int y, uint8_t buttons, int w, int h)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 1;
    fe.pointer[0][RETRO_DEVICE_ID_POINTER_X] = mut_axis(x, w);
    fe.pointer[0][RETRO_DEVICE_ID_POINTER_Y] = mut_axis(y, h);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_LEFT] = (buttons & TABLET_FLAG_LEFT) != 0;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_RIGHT] = (buttons & TABLET_FLAG_RIGHT) != 0;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_MIDDLE] = (buttons & TABLET_FLAG_MIDDLE) != 0;
}

void mut_pointer_gone(void)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    fe.pointer[0][RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN] = 1;
}

mut_budget_t mut_measure(const char *name)
{
    (void)name;
    return MUT_BUDGET_NONE;
}
