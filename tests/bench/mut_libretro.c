/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine under test, when it is a libretro core.
 *
 * The same suites the other two bindings answer, asked through the shipped
 * library instead of through a function call. What the seam adds — the load
 * verdict, the pixel format, the memory interface — is what this binding
 * converts back, so a frame the machine painted is the frame the suite
 * already has an expectation for.
 *
 * Not everything crosses. There is no console here: nothing in the ABI
 * carries the terminal, and exporting a tap so a test could read one would
 * contradict the very claim that the ABI is all a core offers. The suites
 * whose claim is what the machine said keep the binding that can hear it.
 */

#include "mut.h"
#include "retro_fe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The same settle the emulator binding allows: the corpus programs set a
 * canvas and stop, and the console canvas resets a frame late. */
#define MUT_SETTLE_FRAMES 20

static uint32_t mut_fb[640 * 480];

void mut_init(int argc, const char *const argv[])
{
    (void)argc;
    (void)argv;
    fe_open();
    /* The machine's RAM comes up random here as it does on the desktop, and
     * an expectation written against a random fill would be a different
     * number every run. The corpus answers to the reproducible one; every
     * other setting is whatever the core defaults to. */
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
    if (fe.frame_w) /* something is loaded */
        fe.unload_game();
    if (!fe_load(rom))
        return false; /* the core refused the image, and that is the answer */
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
    /* Back the way the core sent it: XRGB8888 to the RGBA8 every expectation
     * in the suite is written against. The exchange is its own inverse, so a
     * frame that does not match here is the core's conversion to answer for,
     * never a number to rewrite. */
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

/* No beam is visible through this ABI, and a frontend's own pacing is not
 * the machine's to be late against. */
mut_budget_t mut_measure(const char *name)
{
    (void)name;
    return MUT_BUDGET_NONE;
}
