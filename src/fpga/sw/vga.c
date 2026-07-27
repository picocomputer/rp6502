/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pocket port of ria/sys/vga.c, before the render side exists: no
 * canvas is connected yet, so callers take their non-VGA fallbacks.
 */

#include "ria/sys/vga.h"

bool vga_connected(void)
{
    return false;
}

vga_canvas_t vga_get_canvas(void)
{
    return vga_canvas_console;
}

uint8_t vga_get_display_type(void)
{
    return 0;
}
