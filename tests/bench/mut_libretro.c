/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

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

mut_budget_t mut_measure(const char *name)
{
    (void)name;
    return MUT_BUDGET_NONE;
}
