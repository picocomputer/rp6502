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

/* The VGA-chip side of the contract, the piece the model still needs:
 * term_view picks the visible terminal by canvas height. */

int16_t vga_canvas_height(void)
{
    return 480;
}

vga_canvas_t vga_get_canvas(void)
{
    return vga_canvas_console;
}

uint8_t vga_get_display_type(void)
{
    return 0;
}
