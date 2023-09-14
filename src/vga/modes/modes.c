/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/modes.h"
#include "sys/pix.h"
#include "sys/vga.h"
#include <stdbool.h>

struct
{
    void (*plane[3])(uint32_t *data, uint16_t scanline);
} scanprog[512];

bool mode0_setup(void)
{
    static uint16_t scanline_max = 0;

    uint16_t plane = pix_xregs[2];
    uint16_t sccanline_begin = pix_xregs[3];
    uint16_t scanline_end = pix_xregs[4];

    if (!scanline_end)
        scanline_end = vga_mode_height();

    for (uint16_t i = sccanline_begin; i < scanline_end; i++)
    {
        scanprog[i].plane[plane] = 0;
    }
}

void mode_setup()
{
    switch (pix_xregs[1])
    {
    case 0:
        break;
    }
}

void mode_init(void)
{
    pix_xregs[1] = 0;
    mode_setup();
}
