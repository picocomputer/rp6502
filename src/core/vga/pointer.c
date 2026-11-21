/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/pointer.h"
#include "core/hid/tablet.h"
#include "core/vga/pixel_format.h"
#include "core/sys/sst.h"
#include "pointer_shapes.h"
#include <string.h>

_Static_assert(POINTER_SHAPE_COUNT == TABLET_CURSOR_COUNT,
               "a pointer shape for each of the tablet's cursor shapes");

/* One size of the shape showing, as pointer_shapes.h has it: 4 bits a
 * pixel, a palette entry, 0 for clear. */
typedef struct
{
    uint8_t width, height, hot_x, hot_y;
    uint8_t data[POINTER_MAX_BYTES];
} pointer_image_t;

/* Two copies of the shape showing, each in both sizes. pointer_set fills the
 * one the renderers are not reading, then points them at it, so a renderer
 * mid-line never sees half a shape: the renderers run on their own core on a
 * Pico, at any time. */
static pointer_image_t pointer_images[2][2]; // [copy][0 for 640 wide, 1 for 320]

/* What the renderers read, each in one word: the shape showing (0 for none)
 * in bits 0-7 and the copy it is in at bit 8; and the hot spot, x in bits
 * 0-15 and y in 16-31. */
static volatile uint32_t pointer_showing;
static volatile uint32_t pointer_where;

/* A palette entry as a pixel, with the alpha bit that puts it over the
 * planes below. */
static uint16_t pointer_pixels[16];

/* The pointer follows the tablet, whose task sets it again as soon as a
 * load is done, so none of it is saved. */
#define POINTER_SST(X)                                                     \
    X(SKIP, pointer_images, "a copy of the shape the tablet's task sets")  \
    X(SKIP, pointer_pixels, "made from the greys at init")                 \
    X(SKIP, pointer_showing, "set by the tablet's task after a load")      \
    X(SKIP, pointer_where, "set by the tablet's task after a load")
SST_TABLE(pointer, POINTER_SST)

void pointer_init(void)
{
    for (int i = 0; i < 16; i++)
    {
        const uint16_t g = POINTER_GREYS[i];
        pointer_pixels[i] = (uint16_t)(SCANVIDEO_PIXEL_FROM_RGB5(g, g, g) | SCANVIDEO_ALPHA_MASK);
    }
    pointer_showing = 0;
    pointer_where = 0;
}

static void pointer_copy(pointer_image_t *to, const pointer_shape_t *from)
{
    to->width = from->width;
    to->height = from->height;
    to->hot_x = from->hot_x;
    to->hot_y = from->hot_y;
    memcpy(to->data, &POINTER_DATA[from->offset], (size_t)((from->width + 1) / 2) * from->height);
}

void pointer_move(int16_t x, int16_t y)
{
    pointer_where = (uint16_t)x | (uint32_t)(uint16_t)y << 16;
}

void pointer_set(uint8_t shape, int16_t x, int16_t y)
{
    pointer_move(x, y);
    pointer_set_shape(shape);
}

void pointer_set_shape(uint8_t shape)
{
    if (shape >= POINTER_SHAPE_COUNT)
        shape = 0;
    const uint32_t now = pointer_showing;
    if ((now & 0xFF) == shape)
        return;
    if (!shape)
    {
        pointer_showing = 0;
        return;
    }
    const unsigned copy = !((now >> 8) & 1);
    pointer_copy(&pointer_images[copy][0], &POINTER_SHAPES[shape][0]);
    pointer_copy(&pointer_images[copy][1], &POINTER_SHAPES[shape][1]);
    pointer_showing = shape | copy << 8;
}

void pointer_render(int16_t y, int16_t width, uint16_t *pixels, bool *filled)
{
    const uint32_t showing = pointer_showing;
    if (!(showing & 0xFF))
        return;
    const pointer_image_t *im = &pointer_images[(showing >> 8) & 1][width <= 320];
    const uint32_t where = pointer_where;
    const int left = (int16_t)(where & 0xFFFF) - im->hot_x;
    const int row = y - ((int16_t)(where >> 16) - im->hot_y);
    if (row < 0 || row >= im->height)
        return;
    if (!*filled)
    {
        memset(pixels, 0, (size_t)width * sizeof(uint16_t));
        *filled = true;
    }
    const uint8_t *src = &im->data[row * ((im->width + 1) / 2)];
    for (int c = 0; c < im->width; c++)
    {
        const int x = left + c;
        if (x < 0 || x >= width)
            continue;
        const unsigned v = (c & 1) ? src[c >> 1] & 0x0F : src[c >> 1] >> 4;
        if (v)
            pixels[x] = pointer_pixels[v];
    }
}
