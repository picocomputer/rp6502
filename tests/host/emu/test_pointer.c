/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The pointer the system draws for the tablet: its shapes, where they land on
 * a scanline, and the tablet putting it where the pointer is. */

#include "core/hid/tablet.h"
#include "core/sys/xram.h"
#include "core/vga/pixel_format.h"
#include "core/vga/pointer.h"
#include "core/vga/vga.h"
#include "host/host.h"
#include "emu_boot.h"
#include "pointer_shapes.h"
#include <string.h>

static uint16_t line[640];

/* The pixel a palette entry of the shapes draws as. */
static uint16_t grey(unsigned entry)
{
    const uint16_t g = POINTER_GREYS[entry];
    return (uint16_t)(SCANVIDEO_PIXEL_FROM_RGB5(g, g, g) | SCANVIDEO_ALPHA_MASK);
}

/* The palette entry of a shape's pixel, size 0 at 640, 1 at 320. */
static unsigned shape_pixel(unsigned shape, unsigned size, int col, int row)
{
    const pointer_shape_t *s = &POINTER_SHAPES[shape][size];
    const uint8_t b = POINTER_DATA[s->offset + row * ((s->width + 1) / 2) + col / 2];
    return (col & 1) ? b & 0x0F : b >> 4;
}

static void render(int16_t y, int16_t width, bool *filled)
{
    pointer_render(y, width, line, filled);
}

/* Every row of every shape: the pixels it has are drawn in their greys, with
 * the alpha bit, at the hot spot's offset, and the rest of the line is left
 * clear. */
UTEST(pointer, a_shape_lands_with_its_hot_spot_on_the_point)
{
    pointer_init();
    for (unsigned shape = 1; shape < POINTER_SHAPE_COUNT; shape++)
        for (unsigned size = 0; size < 2; size++)
        {
            const int16_t width = size ? 320 : 640;
            const pointer_shape_t *s = &POINTER_SHAPES[shape][size];
            pointer_set((uint8_t)shape, 100, 50);
            for (int row = 0; row < s->height; row++)
            {
                memset(line, 0xAA, sizeof line);
                bool filled = false;
                render((int16_t)(50 - s->hot_y + row), width, &filled);
                ASSERT_TRUE(filled);
                for (int x = 0; x < width; x++)
                {
                    const int col = x - (100 - s->hot_x);
                    const unsigned v = col >= 0 && col < s->width ? shape_pixel(shape, size, col, row) : 0;
                    ASSERT_EQ(line[x], v ? grey(v) : 0);
                }
            }
        }
}

/* Off the pointer's rows nothing is drawn, a filled line keeps what it has,
 * and with no shape there is no pointer. */
UTEST(pointer, nothing_is_drawn_off_its_rows_or_with_no_shape)
{
    pointer_init();
    pointer_set(TABLET_CURSOR_ARROW, 100, 50);
    const pointer_shape_t *s = &POINTER_SHAPES[TABLET_CURSOR_ARROW][0];
    bool filled = false;
    memset(line, 0xAA, sizeof line);
    render((int16_t)(50 - s->hot_y - 1), 640, &filled);
    render((int16_t)(50 - s->hot_y + s->height), 640, &filled);
    ASSERT_FALSE(filled);
    ASSERT_EQ(line[100], 0xAAAA);
    filled = true;
    render(50, 640, &filled);
    ASSERT_EQ(line[0], 0xAAAA); /* a filled line is not cleared */
    pointer_set(TABLET_CURSOR_OFF, 100, 50);
    filled = false;
    render(50, 640, &filled);
    ASSERT_FALSE(filled);
}

/* A pointer at an edge is cut off there, never drawn past it. */
UTEST(pointer, a_pointer_at_an_edge_is_cut_off)
{
    pointer_init();
    pointer_set(TABLET_CURSOR_MOVE, 0, 0);
    bool filled = false;
    memset(line, 0, sizeof line);
    render(0, 320, &filled);
    ASSERT_TRUE(filled);
    pointer_set(TABLET_CURSOR_MOVE, 319, 239);
    filled = false;
    render(239, 320, &filled);
    ASSERT_TRUE(filled);
    ASSERT_NE(line[319], 0);
}

/* The tablet puts the pointer where its contact hovers, in the shape the
 * program asked for, while the program has the tablet and the host draws no
 * cursor. Mapping shares the tablet. With no program on it, the console's
 * terminal claims it while mode 0 shows, and the pointer is the arrow. */
UTEST(pointer, the_tablet_shows_the_pointer_where_it_hovers)
{
    pointer_init();
    const uint16_t block = 0x1000;
    ASSERT_TRUE(tablet_xreg(block));
    ASSERT_EQ(xram[block + 1] & TABLET_STATUS_SHARED, TABLET_STATUS_SHARED);
    ASSERT_EQ(xram[block], TABLET_CURSOR_ARROW);

    tablet_host_pointer(200, 100, 0, false);
    tablet_task();
    const pointer_shape_t *arrow = &POINTER_SHAPES[TABLET_CURSOR_ARROW][0];
    bool filled = false;
    memset(line, 0, sizeof line);
    render(100, 640, &filled);
    ASSERT_TRUE(filled);
    ASSERT_NE(line[200 - arrow->hot_x + 0], 0);

    /* Another shape. */
    xram[block] = TABLET_CURSOR_NOT_ALLOWED;
    tablet_task();
    const pointer_shape_t *no = &POINTER_SHAPES[TABLET_CURSOR_NOT_ALLOWED][0];
    filled = false;
    render((int16_t)(100 - no->hot_y), 640, &filled);
    ASSERT_TRUE(filled);

    /* The host draws its own: none here. */
    tablet_host_pointer(200, 100, 0, true);
    tablet_task();
    filled = false;
    render(100, 640, &filled);
    ASSERT_FALSE(filled);
    ASSERT_EQ(xram[block + 1] & TABLET_STATUS_SHARED, TABLET_STATUS_SHARED);

    /* Gone from the window, or no shape. */
    tablet_host_pointer(200, 100, 0, false);
    tablet_host_clear();
    tablet_task();
    filled = false;
    render(100, 640, &filled);
    ASSERT_FALSE(filled);
    tablet_host_pointer(200, 100, 0, false);
    xram[block] = TABLET_CURSOR_OFF;
    tablet_task();
    filled = false;
    render(100, 640, &filled);
    ASSERT_FALSE(filled);

    /* The program lets the tablet go, over the console: the arrow, though
     * the program asked for none. */
    ASSERT_TRUE(tablet_xreg(0xFFFF));
    tablet_task();
    filled = false;
    render(100, 640, &filled);
    ASSERT_TRUE(filled);
    ASSERT_NE(line[200 - arrow->hot_x + 0], 0);

    /* A canvas with no mode 0 on it: no pointer. */
    ASSERT_TRUE(vga_canvas_select(vga_canvas_640_480));
    tablet_task();
    filled = false;
    render(100, 640, &filled);
    ASSERT_FALSE(filled);
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
    tablet_host_clear();
    tablet_task();
}

/* A frame drawn whole: the pointer is over the console's text. */
static uint32_t frame[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

UTEST(pointer, a_frame_shows_the_pointer_over_the_console)
{
    vga_set_framebuffer(frame);
    emu_frames(2);
    ASSERT_TRUE(tablet_xreg(0x1000));
    tablet_host_pointer(300, 200, 0, false);
    tablet_task();
    emu_frames(2);
    int w, h;
    vga_canvas_size(&w, &h);
    const pointer_shape_t *arrow = &POINTER_SHAPES[TABLET_CURSOR_ARROW][w <= 320];
    /* The arrow's white is at its middle, one pixel in from its left edge. */
    const int x = 300 - arrow->hot_x + 1, y = 200 - arrow->hot_y + arrow->height / 2;
    ASSERT_EQ(frame[y * w + x] & 0x00FFFFFFu, 0x00FFFFFFu);
    ASSERT_TRUE(tablet_xreg(0xFFFF));
    tablet_host_clear();
    tablet_task();
    vga_set_framebuffer(NULL);
}

/* The console's rows 12 to 14, where the pointer below is, and nothing that
 * blinks. */
static uint32_t pointer_rows(int w)
{
    return host_crc32(0, &frame[192 * w], (size_t)48 * w * sizeof frame[0]);
}

/* With no program on the tablet, the frame over the console shows the
 * arrow just as a program's mapped tablet shows it. */
UTEST(pointer, a_frame_shows_the_pointer_unmapped_over_the_console)
{
    vga_set_framebuffer(frame);
    int w, h;
    vga_canvas_size(&w, &h);
    tablet_host_clear();
    tablet_task();
    emu_frames(2);
    const uint32_t none = pointer_rows(w);

    tablet_host_pointer(300, 200, 0, false);
    tablet_task();
    emu_frames(2);
    const uint32_t unmapped = pointer_rows(w);
    ASSERT_NE(unmapped, none);

    ASSERT_TRUE(tablet_xreg(0x1000));
    tablet_task();
    emu_frames(2);
    ASSERT_EQ(pointer_rows(w), unmapped);

    ASSERT_TRUE(tablet_xreg(0xFFFF));
    tablet_host_clear();
    tablet_task();
    emu_frames(2);
    ASSERT_EQ(pointer_rows(w), none);
    vga_set_framebuffer(NULL);
}

UTEST_MAIN_EMU();
