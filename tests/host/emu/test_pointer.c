/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The pointer the system draws for the tablet: its shapes, where they land on
 * a scanline, and the tablet putting it where the pointer is. */

#include "core/api/xreg.h"
#include "core/hid/tablet.h"
#include "core/sys/xram.h"
#include "core/vga/mode/mode5.h"
#include "core/vga/pixel_format.h"
#include "core/vga/mode/mode6.h"
#include "core/vga/vga.h"
#include "host/host.h"
#include "emu_boot.h"
#include "mode6_shapes.h"
#include <string.h>

static uint16_t line[640];

/* The pixel a palette entry of the shapes draws as. */
static uint16_t grey(unsigned entry)
{
    const uint16_t g = MODE6_GREYS[entry];
    return (uint16_t)(SCANVIDEO_PIXEL_FROM_RGB5(g, g, g) | SCANVIDEO_ALPHA_MASK);
}

/* A shape's size, as its custom sprite's size byte has it. */
static int shape_width(const mode6_shape_t *s)
{
    return ((s->width_height & 15) + 1) * 4;
}

static int shape_height(const mode6_shape_t *s)
{
    return ((s->width_height >> 4) + 1) * 4;
}

/* The palette entry of a shape's pixel, size 0 at 640, 1 at 320. */
static unsigned shape_pixel(unsigned shape, unsigned size, int col, int row)
{
    const mode6_shape_t *s = &MODE6_SHAPES[shape][size];
    const uint8_t b = MODE6_DATA[s->offset + row * (shape_width(s) / 2) + col / 2];
    return (col & 1) ? b & 0x0F : b >> 4;
}

static void render(int16_t y, int16_t width, bool *filled)
{
    mode6_render(y, width, line, filled);
}

/* Every row of every shape: the pixels it has are drawn in their greys, with
 * the alpha bit, at the hot spot's offset, and the rest of the line is left
 * clear. */
UTEST(pointer, a_shape_lands_with_its_hot_spot_on_the_point)
{
    mode6_init();
    for (unsigned shape = 1; shape < MODE6_SHAPE_COUNT; shape++)
        for (unsigned size = 0; size < 2; size++)
        {
            const int16_t width = size ? 320 : 640;
            const mode6_shape_t *s = &MODE6_SHAPES[shape][size];
            mode6_set((uint8_t)shape, 100, 50);
            for (int row = 0; row < shape_height(s); row++)
            {
                memset(line, 0xAA, sizeof line);
                bool filled = false;
                render((int16_t)(50 - s->hot_y + row), width, &filled);
                ASSERT_TRUE(filled);
                for (int x = 0; x < width; x++)
                {
                    const int col = x - (100 - s->hot_x);
                    const unsigned v = col >= 0 && col < shape_width(s) ? shape_pixel(shape, size, col, row) : 0;
                    ASSERT_EQ(line[x], v ? grey(v) : 0);
                }
            }
        }
}

/* Off the pointer's rows nothing is drawn, a filled line keeps what it has,
 * and with no shape there is no pointer. */
UTEST(pointer, nothing_is_drawn_off_its_rows_or_with_no_shape)
{
    mode6_init();
    mode6_set(TABLET_CURSOR_ARROW, 100, 50);
    const mode6_shape_t *s = &MODE6_SHAPES[TABLET_CURSOR_ARROW][0];
    bool filled = false;
    memset(line, 0xAA, sizeof line);
    render((int16_t)(50 - s->hot_y - 1), 640, &filled);
    render((int16_t)(50 - s->hot_y + shape_height(s)), 640, &filled);
    ASSERT_FALSE(filled);
    ASSERT_EQ(line[100], 0xAAAA);
    filled = true;
    render(50, 640, &filled);
    ASSERT_EQ(line[0], 0xAAAA); /* a filled line is not cleared */
    mode6_set(TABLET_CURSOR_OFF, 100, 50);
    filled = false;
    render(50, 640, &filled);
    ASSERT_FALSE(filled);
}

/* A pointer at an edge is cut off there, never drawn past it. */
UTEST(pointer, a_pointer_at_an_edge_is_cut_off)
{
    mode6_init();
    mode6_set(TABLET_CURSOR_MOVE, 0, 0);
    bool filled = false;
    memset(line, 0, sizeof line);
    render(0, 320, &filled);
    ASSERT_TRUE(filled);
    mode6_set(TABLET_CURSOR_MOVE, 319, 239);
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
    mode6_init();
    const uint16_t block = 0x1000;
    ASSERT_TRUE(tablet_xreg(block));
    ASSERT_EQ(xram[block + 1] & TABLET_STATUS_SHARED, TABLET_STATUS_SHARED);
    ASSERT_EQ(xram[block], TABLET_CURSOR_ARROW);

    tablet_host_pointer(200, 100, 0, false);
    tablet_task();
    const mode6_shape_t *arrow = &MODE6_SHAPES[TABLET_CURSOR_ARROW][0];
    bool filled = false;
    memset(line, 0, sizeof line);
    render(100, 640, &filled);
    ASSERT_TRUE(filled);
    ASSERT_NE(line[200 - arrow->hot_x + 0], 0);

    /* Another shape. */
    xram[block] = TABLET_CURSOR_NOT_ALLOWED;
    tablet_task();
    const mode6_shape_t *no = &MODE6_SHAPES[TABLET_CURSOR_NOT_ALLOWED][0];
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
    const mode6_shape_t *arrow = &MODE6_SHAPES[TABLET_CURSOR_ARROW][w <= 320];
    /* The arrow's white is at its middle, one pixel in from its left edge. */
    const int x = 300 - arrow->hot_x + 1, y = 200 - arrow->hot_y + shape_height(arrow) / 2;
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

/* ---- the program's pointers --------------------------------------------- */

#define CUSTOM_CONFIG 0x2000
#define CUSTOM_IMAGE 0x2100
#define CUSTOM_PALETTE 0x2200
#define CUSTOM_RED ((uint16_t)(SCANVIDEO_PIXEL_FROM_RGB5(31, 0, 0) | SCANVIDEO_ALPHA_MASK))

/* Sprite n of the program's pointers at CUSTOM_CONFIG: an 8x4 sprite of 1
 * bit a pixel, its top row a solid bar and the rest clear, with its hot
 * spot at hot_x, hot_y and its own palette, clear and red. */
static void custom_sprite(unsigned n, int16_t hot_x, int16_t hot_y, uint8_t options)
{
    const mode5_csprite_t s = {
        .x_pos_px = hot_x,
        .y_pos_px = hot_y,
        .xram_sprite_ptr = CUSTOM_IMAGE,
        .palette_ptr = CUSTOM_PALETTE,
        .width_height = 0x01, /* 8 wide, 4 high */
        .options = options,
    };
    memcpy((uint8_t *)&xram[CUSTOM_CONFIG + n * sizeof s], &s, sizeof s);
    const uint8_t image[4] = {0xFF, 0, 0, 0};
    memcpy((uint8_t *)&xram[CUSTOM_IMAGE], image, sizeof image);
    const uint16_t palette[2] = {0, CUSTOM_RED};
    memcpy((uint8_t *)&xram[CUSTOM_PALETTE], palette, sizeof palette);
}

/* Programs mode 6, as a program's XREG calls do. */
static bool mode6(uint16_t config)
{
    return xreg1(0, 2, config) && xreg1(0, 1, 6);
}

/* The red bar the line holds: its first pixel and how many, 0 for none. */
static int bar(int16_t y, int16_t width, int *left)
{
    memset(line, 0, sizeof line);
    bool filled = false;
    render(y, width, &filled);
    int n = 0;
    *left = -1;
    for (int x = 0; x < width; x++)
        if (line[x] == CUSTOM_RED)
        {
            if (*left < 0)
                *left = x;
            n++;
        }
    return n;
}

/* Shape n draws sprite n - 1 with its hot spot on the pointer, at its own
 * size on a 320 canvas too, and a shape is any number a sprite is there
 * for. */
UTEST(pointer, a_programs_shape_n_draws_its_sprite_n_minus_1)
{
    mode6_init();
    ASSERT_TRUE(vga_canvas_select(vga_canvas_640_480));
    custom_sprite(0, 0, 0, 0);
    custom_sprite(199, 3, 2, 0);
    ASSERT_TRUE(mode6(CUSTOM_CONFIG));
    ASSERT_TRUE(mode6_custom());
    int left;
    mode6_set(1, 100, 50);
    ASSERT_EQ(bar(50, 640, &left), 8);
    ASSERT_EQ(left, 100);
    mode6_set(200, 100, 50);
    ASSERT_EQ(bar(48, 640, &left), 8);
    ASSERT_EQ(left, 97);
    ASSERT_EQ(bar(50, 640, &left), 0);
    ASSERT_TRUE(vga_canvas_select(vga_canvas_320_240));
    ASSERT_TRUE(mode6_custom()); /* a change of the program's canvas keeps them */
    ASSERT_EQ(bar(48, 320, &left), 8);
    ASSERT_EQ(left, 97);
    /* Doubled down and across. */
    custom_sprite(199, 3, 2, 0x40 | 0x80);
    ASSERT_EQ(bar(49, 320, &left), 16);
    ASSERT_EQ(left, 97);
    ASSERT_EQ(bar(50, 320, &left), 0);
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
}

/* A shape whose sprite, or that sprite's image, would run past the end of
 * XRAM draws nothing. */
UTEST(pointer, a_sprite_past_the_end_of_xram_draws_nothing)
{
    mode6_init();
    ASSERT_TRUE(vga_canvas_select(vga_canvas_640_480));
    ASSERT_TRUE(mode6(0xFFEC)); /* room for sprites 0 and 1 only */
    const mode5_csprite_t s = {.xram_sprite_ptr = CUSTOM_IMAGE, .palette_ptr = CUSTOM_PALETTE,
                               .width_height = 0x01};
    memcpy((uint8_t *)&xram[0xFFEC], &s, sizeof s);
    memcpy((uint8_t *)&xram[0xFFF6], &s, sizeof s);
    custom_sprite(0, 0, 0, 0); /* the image and the palette */
    int left;
    mode6_set(2, 100, 50);
    ASSERT_EQ(bar(50, 640, &left), 8);
    mode6_set(3, 100, 50);
    ASSERT_EQ(bar(50, 640, &left), 0);
    mode6_set(255, 100, 50);
    ASSERT_EQ(bar(50, 640, &left), 0);
    ASSERT_TRUE(mode6(CUSTOM_CONFIG));
    custom_sprite(0, 0, 0, 0);
    xram[CUSTOM_CONFIG + 4] = 0xFF; /* the image at $FFFF */
    xram[CUSTOM_CONFIG + 5] = 0xFF;
    mode6_set(1, 100, 50);
    ASSERT_EQ(bar(50, 640, &left), 0);
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
}

/* Mode 6 is refused on the console and on an odd address; $FFFF, and a
 * return to the console, put the system's pointers back. With them, a shape
 * the system has none of draws nothing. */
UTEST(pointer, the_console_has_the_systems_pointers)
{
    mode6_init();
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
    ASSERT_FALSE(mode6(CUSTOM_CONFIG));
    ASSERT_FALSE(mode6_custom());
    ASSERT_TRUE(vga_canvas_select(vga_canvas_640_480));
    ASSERT_FALSE(mode6(CUSTOM_CONFIG + 1));
    ASSERT_FALSE(mode6_custom());
    ASSERT_TRUE(mode6(CUSTOM_CONFIG));
    ASSERT_TRUE(mode6(0xFFFF));
    ASSERT_FALSE(mode6_custom());
    ASSERT_TRUE(mode6(CUSTOM_CONFIG));
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
    ASSERT_FALSE(mode6_custom());
    mode6_set(TABLET_CURSOR_COUNT, 100, 50);
    bool filled = false;
    render(50, 640, &filled);
    ASSERT_FALSE(filled);
}

/* A host that shows its own cursor for the system's shapes leaves the
 * program's pointers to the VGA, and the tablet passes on any shape. */
UTEST(pointer, a_hosts_cursor_gives_way_to_the_programs_pointers)
{
    mode6_init();
    ASSERT_TRUE(vga_canvas_select(vga_canvas_640_480));
    custom_sprite(19, 0, 0, 0);
    const uint16_t block = 0x1000;
    ASSERT_TRUE(tablet_xreg(block));
    xram[block] = 20;
    tablet_host_pointer(200, 100, 0, true);
    tablet_task();
    int left;
    ASSERT_EQ(bar(100, 640, &left), 0); /* no sprite 19 yet: the host's */
    ASSERT_TRUE(mode6(CUSTOM_CONFIG));
    tablet_task();
    ASSERT_EQ(bar(100, 640, &left), 8);
    ASSERT_EQ(left, 200);
    ASSERT_TRUE(tablet_xreg(0xFFFF));
    tablet_host_clear();
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
    tablet_task();
}

UTEST_MAIN_EMU();
