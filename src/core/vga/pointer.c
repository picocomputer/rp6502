/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/pointer.h"
#include "core/hid/tablet.h"
#include "core/sys/sst.h"
#include "core/sys/xram.h"
#include "core/vga/mode/mode5.h"
#include "core/vga/pixel_format.h"
#include "pointer_shapes.h"
#include <string.h>

_Static_assert(POINTER_SHAPE_COUNT == TABLET_CURSOR_COUNT,
               "a pointer shape for each of the tablet's cursor shapes");

/* The options byte of the system's shapes: 4 bits a pixel. */
#define POINTER_OPTIONS 2

/* One size of a system shape, as pointer_shapes.h has it. */
typedef struct
{
    uint8_t hot_x, hot_y, width_height;
    uint8_t data[POINTER_MAX_BYTES];
} pointer_image_t;

/* Two copies of the system shape last set, each in both sizes.
 * pointer_set_shape fills the one the renderers are not reading, then
 * points them at it, so a renderer mid-line never sees half a shape: the
 * renderers run on their own core on a Pico, at any time. */
static pointer_image_t pointer_images[2][2]; // [copy][0 for 640 wide, 1 for 320]

/* The system shape in the copy the renderers read, 0 for none. */
static uint8_t pointer_copied;

/* What the renderers read, each in one word: the shape (0 for none) in
 * bits 0-7 and the copy of the system's shapes to draw from at bit 8; the
 * hot spot, x in bits 0-15 and y in 16-31; and the program's pointers. */
static volatile uint32_t pointer_showing;
static volatile uint32_t pointer_where;
static volatile uint16_t pointer_config;

/* The palette of the system's shapes: its greys as pixels, with the alpha
 * bit that puts them over the planes below, and entry 0 clear. */
static uint16_t pointer_pixels[16];

/* The pointer follows the tablet, whose task sets it again as soon as a
 * load is done, so only the program's pointers are saved. */
#define POINTER_SST(X)                                                     \
    X(SKIP, pointer_images, "a copy of the shape the tablet's task sets")  \
    X(SKIP, pointer_copied, "set with pointer_images")                     \
    X(SKIP, pointer_pixels, "made from the greys at init")                 \
    X(SKIP, pointer_showing, "set by the tablet's task after a load")      \
    X(SKIP, pointer_where, "set by the tablet's task after a load")        \
    X(U16, pointer_config)
SST_TABLE(pointer, POINTER_SST)

void pointer_init(void)
{
    for (int i = 1; i < 16; i++)
    {
        const uint16_t g = POINTER_GREYS[i];
        pointer_pixels[i] = (uint16_t)(SCANVIDEO_PIXEL_FROM_RGB5(g, g, g) | SCANVIDEO_ALPHA_MASK);
    }
    pointer_pixels[0] = 0;
    pointer_copied = 0;
    pointer_showing = 0;
    pointer_where = 0;
    pointer_config = 0xFFFF;
}

static void pointer_copy(pointer_image_t *to, const pointer_shape_t *from)
{
    to->hot_x = from->hot_x;
    to->hot_y = from->hot_y;
    to->width_height = from->width_height;
    memcpy(to->data, &POINTER_DATA[from->offset],
           mode5_custom_bytes(from->width_height, POINTER_OPTIONS));
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
    unsigned copy = (pointer_showing >> 8) & 1;
    if (shape && shape < POINTER_SHAPE_COUNT && shape != pointer_copied)
    {
        copy = !copy;
        pointer_copy(&pointer_images[copy][0], &POINTER_SHAPES[shape][0]);
        pointer_copy(&pointer_images[copy][1], &POINTER_SHAPES[shape][1]);
        pointer_copied = shape;
    }
    pointer_showing = shape | copy << 8;
}

void pointer_set_custom(uint16_t config)
{
    pointer_config = config;
}

bool pointer_custom(void)
{
    return pointer_config != 0xFFFF;
}

void pointer_render(int16_t y, int16_t width, uint16_t *pixels, bool *filled)
{
    const uint32_t showing = pointer_showing;
    const unsigned shape = showing & 0xFF;
    if (!shape)
        return;
    const uint32_t where = pointer_where;
    const uint16_t config = pointer_config;
    int hot_x, hot_y;
    uint8_t width_height, options;
    const uint8_t *image;
    const uint16_t *palette;
    if (config != 0xFFFF)
    {
        /* Each field is read out of XRAM once, checked, and drawn from that
         * copy, as mode 5 does. */
        const uint32_t at = config + (shape - 1) * (uint32_t)sizeof(mode5_csprite_t);
        if (at > 0x10000 - sizeof(mode5_csprite_t))
            return;
        const mode5_csprite_t *s = (const mode5_csprite_t *)&xram[at];
        hot_x = s->x_pos_px;
        hot_y = s->y_pos_px;
        width_height = s->width_height;
        options = s->options;
        const uint16_t image_ptr = s->xram_sprite_ptr;
        const uint16_t palette_ptr = s->palette_ptr;
        if (image_ptr > 0x10000 - mode5_custom_bytes(width_height, options))
            return;
        image = (const uint8_t *)&xram[image_ptr];
        palette = mode5_palette(palette_ptr, options);
    }
    else
    {
        if (shape >= POINTER_SHAPE_COUNT)
            return;
        const pointer_image_t *im = &pointer_images[(showing >> 8) & 1][width <= 320];
        hot_x = im->hot_x;
        hot_y = im->hot_y;
        width_height = im->width_height;
        options = POINTER_OPTIONS;
        image = im->data;
        palette = pointer_pixels;
    }
    const int height = ((width_height >> 4) + 1) * 4 << (options >> 7);
    const int row = y - ((int16_t)(where >> 16) - hot_y);
    if ((unsigned)row >= (unsigned)height)
        return;
    if (!*filled)
    {
        memset(pixels, 0, (size_t)width * sizeof(uint16_t));
        *filled = true;
    }
    mode5_custom_line((int16_t)(where & 0xFFFF) - hot_x, row, width_height, options,
                      image, palette, width, pixels);
}
