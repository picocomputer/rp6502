/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/mode0.h"
#include "sys/pix.h"
#include "sys/vga.h"
/*

__attribute__((optimize("O1"))) void
term_render_mode0_640(uint32_t *data, uint16_t scanline)
{
    term_state_t *term = &term_console;
    if (vga_mode_height() <= term->y * 16) // TODO
        scanline += term->y * 16 - vga_mode_height() + 16;
    const uint8_t *font_line = &font16[(scanline & 15) * 256];
    scanline = scanline / 16 + term->y_offset;
    if (scanline >= term->height)
        scanline -= term->height;
    struct cell_state *term_ptr = term->mem + term->width * scanline;
    data--;
    for (int i = 0; i < term->width; i++, term_ptr++)
    {
        uint8_t bits = font_line[term_ptr->glyph_code];
        uint16_t fg = term_ptr->fg_color;
        uint16_t bg = term_ptr->bg_color;
        switch (bits >> 4)
        {
        case 0:
            *++data = bg | (bg << 16);
            *++data = bg | (bg << 16);
            break;
        case 1:
            *++data = bg | (bg << 16);
            *++data = bg | (fg << 16);
            break;
        case 2:
            *++data = bg | (bg << 16);
            *++data = fg | (bg << 16);
            break;
        case 3:
            *++data = bg | (bg << 16);
            *++data = fg | (fg << 16);
            break;
        case 4:
            *++data = bg | (fg << 16);
            *++data = bg | (bg << 16);
            break;
        case 5:
            *++data = bg | (fg << 16);
            *++data = bg | (fg << 16);
            break;
        case 6:
            *++data = bg | (fg << 16);
            *++data = fg | (bg << 16);
            break;
        case 7:
            *++data = bg | (fg << 16);
            *++data = fg | (fg << 16);
            break;
        case 8:
            *++data = fg | (bg << 16);
            *++data = bg | (bg << 16);
            break;
        case 9:
            *++data = fg | (bg << 16);
            *++data = bg | (fg << 16);
            break;
        case 10:
            *++data = fg | (bg << 16);
            *++data = fg | (bg << 16);
            break;
        case 11:
            *++data = fg | (bg << 16);
            *++data = fg | (fg << 16);
            break;
        case 12:
            *++data = fg | (fg << 16);
            *++data = bg | (bg << 16);
            break;
        case 13:
            *++data = fg | (fg << 16);
            *++data = bg | (fg << 16);
            break;
        case 14:
            *++data = fg | (fg << 16);
            *++data = fg | (bg << 16);
            break;
        case 15:
            *++data = fg | (fg << 16);
            *++data = fg | (fg << 16);
            break;
        }
        switch (bits & 0xF)
        {
        case 0:
            *++data = bg | (bg << 16);
            *++data = bg | (bg << 16);
            break;
        case 1:
            *++data = bg | (bg << 16);
            *++data = bg | (fg << 16);
            break;
        case 2:
            *++data = bg | (bg << 16);
            *++data = fg | (bg << 16);
            break;
        case 3:
            *++data = bg | (bg << 16);
            *++data = fg | (fg << 16);
            break;
        case 4:
            *++data = bg | (fg << 16);
            *++data = bg | (bg << 16);
            break;
        case 5:
            *++data = bg | (fg << 16);
            *++data = bg | (fg << 16);
            break;
        case 6:
            *++data = bg | (fg << 16);
            *++data = fg | (bg << 16);
            break;
        case 7:
            *++data = bg | (fg << 16);
            *++data = fg | (fg << 16);
            break;
        case 8:
            *++data = fg | (bg << 16);
            *++data = bg | (bg << 16);
            break;
        case 9:
            *++data = fg | (bg << 16);
            *++data = bg | (fg << 16);
            break;
        case 10:
            *++data = fg | (bg << 16);
            *++data = fg | (bg << 16);
            break;
        case 11:
            *++data = fg | (bg << 16);
            *++data = fg | (fg << 16);
            break;
        case 12:
            *++data = fg | (fg << 16);
            *++data = bg | (bg << 16);
            break;
        case 13:
            *++data = fg | (fg << 16);
            *++data = bg | (fg << 16);
            break;
        case 14:
            *++data = fg | (fg << 16);
            *++data = fg | (bg << 16);
            break;
        case 15:
            *++data = fg | (fg << 16);
            *++data = fg | (fg << 16);
            break;
        }
    }
}

*/
