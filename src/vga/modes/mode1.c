/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/mode1.h"
#include "sys/pix.h"
#include "sys/vga.h"
#include "sys/xram.h"
#include "term/color.h"
#include "term/font.h"
#include "pico/scanvideo.h"

__attribute__((optimize("O1"))) void
mode1_256_00_8x16_640x480(mode1_config_t *config, int16_t scanline, uint32_t *data)
{
    const uint8_t *font_line;
    if (config->xram_font_ptr == 0xFFFF)
        font_line = &font16[(scanline & 15) * 256];
    else
        font_line = &((const uint8_t *)&xram[config->xram_font_ptr])[(scanline & 15) * 256];

    scanline += config->ypos_px;
    if (scanline < 0 || scanline > config->height_chars * 16)
    {
        for (int i = 0; i < 320; i++)
            data[i] = PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 0) |
                      PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 0) << 16;
        return;
    }

    mode1_16_data_t *cell_ptr = (mode1_16_data_t *)&xram[config->xram_data_ptr] + scanline * 640;

    int start = 0; // TODO
    int end = 0;   // TODO

    data--;
    for (int i = start; i < end; i++, cell_ptr++)
    {
        uint8_t bits = font_line[cell_ptr->glyph_code];
        uint16_t fg = cell_ptr->fg_color;
        uint16_t bg = cell_ptr->bg_color;
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
