/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/vga.h"
#include "core/vga/vga_emu.h"
#include "core/vga/prog.h"
#include "core/sys/xram.h"
#include "core/vga/mode/mode0.h"
#include "core/vga/mode/mode1.h"
#include "core/vga/mode/mode2.h"
#include "core/vga/mode/mode3.h"
#include "core/vga/mode/mode4.h"
#include "core/vga/mode/mode5.h"
#include "emu_boot.h"
#include "utest.h"

#include <string.h>

/* Mode 2 is left out because all of its attributes share one renderer, so
 * vga_mode_fill_id reads a mode 2 attribute from the row and plane rather
 * than from the renderer. */
static const struct
{
    uint8_t mode;
    uint16_t attr;
} fills[] = {
    {0, 0},
    {1, 0}, {1, 1}, {1, 2}, {1, 3}, {1, 4},
    {1, 8}, {1, 9}, {1, 10}, {1, 11}, {1, 12},
    {3, 0}, {3, 1}, {3, 2}, {3, 3}, {3, 4},
    {3, 8}, {3, 9}, {3, 10},
};

static const struct
{
    uint8_t mode;
    uint16_t attr;
} sprites[] = {
    {4, 0}, {4, 1},
    {5, 0}, {5, 1}, {5, 2}, {5, 3},
    {5, 8}, {5, 9}, {5, 10}, {5, 11},
    {5, 16}, {5, 17}, {5, 18}, {5, 19},
    {5, 24}, {5, 25}, {5, 26}, {5, 27},
    {5, 32}, {5, 33}, {5, 34}, {5, 35},
    {5, 40}, {5, 41}, {5, 42}, {5, 43},
    {5, 48}, {5, 49}, {5, 56},
};

#define FILL_COUNT (sizeof fills / sizeof fills[0])
#define SPRITE_COUNT (sizeof sprites / sizeof sprites[0])

UTEST(progmap, every_fill_attribute_comes_back)
{
    for (size_t i = 0; i < FILL_COUNT; i++)
    {
        vga_fill_fn_t fn = vga_mode_fill_fn(fills[i].mode, fills[i].attr);
        ASSERT_TRUE(fn != NULL);
        uint8_t mode = VGA_MODE_NONE;
        uint16_t attr = 0xFFFF;
        ASSERT_TRUE(vga_mode_fill_id(fn, 0, 0, &mode, &attr));
        ASSERT_EQ((int)mode, (int)fills[i].mode);
        ASSERT_EQ((int)attr, (int)fills[i].attr);
    }
}

UTEST(progmap, every_sprite_attribute_comes_back)
{
    for (size_t i = 0; i < SPRITE_COUNT; i++)
    {
        vga_sprite_fn_t fn = vga_mode_sprite_fn(sprites[i].mode, sprites[i].attr);
        ASSERT_TRUE(fn != NULL);
        uint8_t mode = VGA_MODE_NONE;
        uint16_t attr = 0xFFFF;
        ASSERT_TRUE(vga_mode_sprite_id(fn, &mode, &attr));
        ASSERT_EQ((int)mode, (int)sprites[i].mode);
        ASSERT_EQ((int)attr, (int)sprites[i].attr);
    }
}

UTEST(progmap, mode_two_classes_come_back_by_row)
{
    static const uint16_t options[] = {0, 1, 2, 3, 8, 9, 10, 11};
    vga_fill_fn_t fn = vga_mode_fill_fn(2, 0);
    ASSERT_TRUE(fn != NULL);
    for (unsigned i = 0; i < 8; i++)
    {
        mode2_set_options((int16_t)i, 0, options[i]);
        uint8_t mode = VGA_MODE_NONE;
        uint16_t attr = 0xFFFF;
        ASSERT_TRUE(vga_mode_fill_id(fn, (int16_t)i, 0, &mode, &attr));
        ASSERT_EQ((int)mode, 2);
        ASSERT_EQ((int)attr, (int)options[i]);
    }
    for (unsigned i = 0; i < 8; i++)
        mode2_set_options((int16_t)i, 0, 0);
}

UTEST(progmap, every_custom_attribute_is_the_canonical_one)
{
    vga_sprite_fn_t custom = vga_mode_sprite_fn(5, 56);
    ASSERT_TRUE(custom != NULL);
    for (unsigned a = 57; a <= 63; a++)
    {
        ASSERT_TRUE(vga_mode_sprite_fn(5, (uint16_t)a) == custom);
        uint8_t mode = VGA_MODE_NONE;
        uint16_t attr = 0xFFFF;
        ASSERT_TRUE(vga_mode_sprite_id(vga_mode_sprite_fn(5, (uint16_t)a), &mode, &attr));
        ASSERT_EQ((int)mode, 5);
        ASSERT_EQ((int)attr, 56);
    }
}

UTEST(progmap, an_empty_row_is_a_mode)
{
    uint8_t mode = 0;
    uint16_t attr = 0xFFFF;
    ASSERT_TRUE(vga_mode_fill_id(NULL, 0, 0, &mode, &attr));
    ASSERT_EQ((int)mode, VGA_MODE_NONE);
    ASSERT_EQ((int)attr, 0);
    ASSERT_TRUE(vga_mode_sprite_id(NULL, &mode, &attr));
    ASSERT_EQ((int)mode, VGA_MODE_NONE);
    ASSERT_EQ((int)attr, 0);
    ASSERT_TRUE(vga_mode_fill_fn(VGA_MODE_NONE, 0) == NULL);
    ASSERT_TRUE(vga_mode_sprite_fn(VGA_MODE_NONE, 0) == NULL);
}

UTEST(progmap, no_two_renderers_are_the_same_address)
{
    void *seen[FILL_COUNT + SPRITE_COUNT];
    size_t n = 0;
    for (size_t i = 0; i < FILL_COUNT; i++)
        seen[n++] = (void *)vga_mode_fill_fn(fills[i].mode, fills[i].attr);
    for (size_t i = 0; i < SPRITE_COUNT; i++)
        seen[n++] = (void *)vga_mode_sprite_fn(sprites[i].mode, sprites[i].attr);
    ASSERT_EQ(n, FILL_COUNT + SPRITE_COUNT);
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            ASSERT_TRUE(seen[i] != seen[j]);
}

UTEST(progmap, validity_and_lookup_are_the_same_list)
{
    for (unsigned a = 0; a <= 0xFF; a++)
    {
        ASSERT_TRUE(mode1_fill_valid((uint16_t)a) == (vga_mode_fill_fn(1, (uint16_t)a) != NULL));
        ASSERT_TRUE(mode3_fill_valid((uint16_t)a) == (vga_mode_fill_fn(3, (uint16_t)a) != NULL));
        ASSERT_TRUE(mode4_sprite_valid((uint16_t)a) == (vga_mode_sprite_fn(4, (uint16_t)a) != NULL));
        ASSERT_TRUE(mode5_sprite_valid((uint16_t)a) == (vga_mode_sprite_fn(5, (uint16_t)a) != NULL));
    }
}

UTEST(progmap, a_renderer_that_is_not_there_is_refused)
{
    ASSERT_TRUE(vga_mode_fill_fn(6, 0) == NULL);
    ASSERT_TRUE(vga_mode_fill_fn(4, 0) == NULL);
    ASSERT_TRUE(vga_mode_sprite_fn(3, 0) == NULL);
    ASSERT_TRUE(vga_mode_fill_fn(1, 5) == NULL);
    ASSERT_TRUE(vga_mode_fill_fn(3, 11) == NULL);
    ASSERT_TRUE(vga_mode_fill_fn(2, 0x8000) == NULL);
    ASSERT_TRUE(vga_mode_sprite_fn(5, 50) == NULL);
    ASSERT_TRUE(vga_mode_sprite_fn(5, 64) == NULL);
    ASSERT_TRUE(vga_mode_sprite_fn(4, 2) == NULL);
}

/* Programs the terminal on plane 0 of a 640x480 canvas. */
static bool terminal_at(int16_t begin, int16_t end)
{
    uint16_t xregs[8] = {0, 0, 0, (uint16_t)begin, (uint16_t)end};
    return mode0_prog(xregs);
}

/* Each terminal row carries the scanline the terminal begins at, so a render
 * on another core has it from the same copy of the row as its renderer, and
 * the old rows of a move hold their own begin rather than the new one. */
UTEST(progmap, a_terminal_row_holds_its_begin)
{
    ASSERT_TRUE(vga_canvas_select(vga_canvas_640_480));
    const vga_fill_fn_t term = mode0_fill_fn(0);
    ASSERT_TRUE(terminal_at(400, 480));
    for (int16_t s = 0; s < 480; s++)
    {
        const vga_prog_t *row = vga_prog_row(s);
        ASSERT_EQ(row->fill_fn[0] == term, s >= 400);
        if (s >= 400)
            ASSERT_EQ(row->fill_config[0], 400);
    }
    ASSERT_TRUE(terminal_at(0, 0));
    for (int16_t s = 0; s < 480; s++)
    {
        ASSERT_TRUE(vga_prog_row(s)->fill_fn[0] == term);
        ASSERT_EQ(vga_prog_row(s)->fill_config[0], 0);
    }
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
}

/* A row copied part way through a move up pairs a scanline above the
 * terminal with a begin below it. The renderer draws nothing for it rather
 * than read a row past the terminal's. */
UTEST(progmap, a_terminal_row_past_the_terminal_draws_nothing)
{
    static uint16_t line[640];
    ASSERT_TRUE(vga_canvas_select(vga_canvas_640_480));
    ASSERT_TRUE(terminal_at(0, 0));
    const vga_fill_fn_t term = mode0_fill_fn(0);
    static vga_fill_cache_t cache;
    ASSERT_FALSE(term(0, 100, 640, line, 400, &cache));
    ASSERT_FALSE(term(0, 479, 640, line, 0xFFF0, &cache));
    ASSERT_TRUE(term(0, 479, 640, line, 0, &cache));
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
}

/* A 4bpp plane keeps its table of every byte's two pixels in the caller's
 * cache from line to line. A palette written between lines, or a change of
 * bit order, still shows on the very next line. */
UTEST(progmap, a_4bpp_palette_change_shows_on_the_next_line)
{
    static uint16_t line[640];
    static vga_fill_cache_t cache;
    /* 640 by 1 at 4bpp: data at 0x1000, palette at 0x2000. */
    const uint8_t cfg[14] = {0, 0, 0, 0, 0, 0, 0x80, 0x02, 1, 0, 0x00, 0x10, 0x00, 0x20};
    for (unsigned i = 0; i < sizeof cfg; i++)
        xram[0x100 + i] = cfg[i];
    for (unsigned i = 0; i < 320; i++)
        xram[0x1000 + i] = 0x12; /* color 1 then color 2 */
    const uint16_t first[3] = {0, 0x1111 | 0x20, 0x2222 | 0x20};
    for (unsigned i = 0; i < 3; i++)
    {
        xram[0x2000 + 2 * i] = (uint8_t)first[i];
        xram[0x2001 + 2 * i] = (uint8_t)(first[i] >> 8);
    }
    const vga_fill_fn_t fill = vga_mode_fill_fn(3, 2);
    const vga_fill_fn_t fill_reverse = vga_mode_fill_fn(3, 10);
    ASSERT_TRUE(fill(0, 0, 640, line, 0x100, &cache));
    ASSERT_EQ(line[0], first[1]);
    ASSERT_EQ(line[639], first[2]);
    const uint16_t second = 0x3333 | 0x20;
    xram[0x2002] = (uint8_t)second;
    xram[0x2003] = (uint8_t)(second >> 8);
    ASSERT_TRUE(fill(0, 0, 640, line, 0x100, &cache));
    ASSERT_EQ(line[0], second);
    ASSERT_EQ(line[639], first[2]);
    ASSERT_TRUE(fill_reverse(0, 0, 640, line, 0x100, &cache));
    ASSERT_EQ(line[0], first[2]);
    ASSERT_EQ(line[639], second);
}

UTEST_MAIN_EMU()
