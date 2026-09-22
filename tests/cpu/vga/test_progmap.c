/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/vga.h"
#include "core/vga/vga_emu.h"
#include "core/vga/prog.h"
#include "core/vga/mode/mode1.h"
#include "core/vga/mode/mode2.h"
#include "core/vga/mode/mode3.h"
#include "core/vga/mode/mode4.h"
#include "core/vga/mode/mode5.h"
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
    ASSERT_TRUE(vga_mode_fill_fn(2, 0x1000) == NULL);
    ASSERT_TRUE(vga_mode_sprite_fn(5, 50) == NULL);
    ASSERT_TRUE(vga_mode_sprite_fn(5, 64) == NULL);
    ASSERT_TRUE(vga_mode_sprite_fn(4, 2) == NULL);
}

UTEST_MAIN();
