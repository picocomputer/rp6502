/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/modes.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "sys/vga.h"
#include "term/term.h"
#include <string.h>
#include <stdbool.h>

scanprog_t scanprog[SCANPROG_MAX];

bool mode0_setup(uint16_t *xregs)
{
    uint16_t plane = xregs[2];
    uint16_t sccanline_begin = xregs[3];
    uint16_t scanline_end = xregs[4];

    if (!scanline_end)
        scanline_end = vga_mode_height();

    for (uint16_t i = sccanline_begin; i < scanline_end; i++)
    {
        scanprog[i].fn640[plane] = term_render_640;
        scanprog[i].ctx[plane] = 0; // term40/80
    }

    return true;
}

bool mode_mode(uint16_t *xregs)
{
    switch (xregs[1])
    {
    case 0:
        return mode0_setup(xregs);
    default:
        return false;
    }
}

void mode_init(void)
{
}
