/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/mode3.h"
#include "pico/scanvideo.h"

/*

static void vga_render_4bpp(struct scanvideo_scanline_buffer *dest)
{
    static const uint32_t colors[] = {
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(205, 0, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 205, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(205, 205, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 205),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(205, 0, 205),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 205, 205),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(229, 229, 229),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(127, 127, 127),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 0, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 255, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 255),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 0, 255),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 255, 255),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 255),
    };
    int line = scanvideo_scanline_number(dest->scanline_id);
    uint8_t *data = xram + line * 160;
    uint16_t *pbuf = (void *)dest->data;
    ++pbuf;
    for (int i = 0; i < 160;)
    {
        *++pbuf = colors[(*data) & 0xF];
        *++pbuf = colors[(*data) >> 4];
        ++data;
        ++i;
    }
    uint32_t *buf = (void *)dest->data;
    buf[0] = COMPOSABLE_RAW_RUN | (buf[1] << 16);
    buf[1] = 317 | (buf[1] & 0xFFFF0000);
    buf[161] = COMPOSABLE_RAW_1P | (0 << 16);
    buf[162] = COMPOSABLE_EOL_SKIP_ALIGN;
    dest->data_used = 163;

    dest->data2[0] = COMPOSABLE_RAW_1P | (0 << 16);
    dest->data2[1] = COMPOSABLE_EOL_SKIP_ALIGN;
    dest->data2_used = 2;

    dest->data3[0] = COMPOSABLE_RAW_1P | (0 << 16);
    dest->data3[1] = COMPOSABLE_EOL_SKIP_ALIGN;
    dest->data3_used = 2;

    dest->status = SCANLINE_OK;
}

*/
