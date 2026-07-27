/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pocket port of ria/sys/vga.c, before the render side exists: no
 * canvas is connected yet, so callers take their non-VGA fallbacks.
 */

#include "mmio.h"

#include "ria/sys/vga.h"

#include <stdint.h>

bool vga_connected(void)
{
    return false;
}

/* The VGA-chip side of the contract (vga/sys/vga.h), carried alongside the
 * RIA side the way emu/sys/vga.c does — matching signatures, no include,
 * because the two headers each define their own vga_canvas_t. */

int16_t vga_canvas_height(void)
{
    return 480;
}

bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin,
                        int16_t scanline_end, uint16_t config_ptr,
                        bool (*fill_fn)(int16_t, int16_t, int16_t,
                                        uint16_t *, uint16_t))
{
    (void)plane;  /* one plane until the mode renderers arrive */
    (void)config_ptr;
    (void)fill_fn;
    VID_PROG = 0x80000000u | ((uint32_t)(uint16_t)scanline_end << 16)
               | (uint16_t)scanline_begin;
    return true;
}

vga_canvas_t vga_get_canvas(void)
{
    return vga_canvas_console;
}

uint8_t vga_get_display_type(void)
{
    return 0;
}
