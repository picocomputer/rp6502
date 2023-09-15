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

bool mode0_setup(void)
{
    uint16_t plane = pix_xregs[2];
    uint16_t sccanline_begin = pix_xregs[3];
    uint16_t scanline_end = pix_xregs[4];

    if (!scanline_end)
        scanline_end = vga_mode_height();

    for (uint16_t i = sccanline_begin; i < scanline_end; i++)
    {
        scanprog[i].fn640[plane] = term_render_640;
        scanprog[i].ctx[plane] = 0; // term40/80
    }

    ria_ack();
}

void mode_mode()
{
    switch (pix_xregs[1])
    {
    case 0:
        break;
    }
    for (int i = 0; i < PIX_XREGS_MAX; i++)
        pix_xregs[i] = 0;
}

void mode_canvas()
{
    memset(&scanprog, 0, sizeof(scanprog));

    switch (pix_xregs[0])
    {
    case 0: // 80 column console. (4:3 or 5:4)
        // if display == sxga then vga_640_512
        vga_resolution(vga_640_360);
        pix_xregs[2] = pix_xregs[3] = pix_xregs[4] = 0;
        mode_mode(); // acks
        break;
    case 1: // 320x240 (4:3)
        vga_resolution(vga_320_240);
        ria_ack();
        break;
    case 2: // 320x180 (16:9)
        vga_resolution(vga_320_180);
        break;
    case 3: // 640x480 (4:3)
        vga_resolution(vga_640_480);
        break;
    case 4: // 640x360 (16:9)
        vga_resolution(vga_640_360);
        break;
    default:
        ria_nak();
        break;
    }
    for (int i = 0; i < PIX_XREGS_MAX; i++)
        pix_xregs[i] = 0;
}

void mode_display(vga_display_t display)
{
    vga_terminal(true);
    vga_display(display);
}

void mode_init(void)
{
    mode_mode();
}
