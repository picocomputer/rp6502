/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/manifold.h"
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
#include "core/term/color.h"
#include "core/term/font.h"
#include "core/term/term.h"
#include "emu_boot.h"
#include "utest.h"

#include <stdio.h>
#include <string.h>

/* Mode 2 is apart because every one of its options has the one renderer. */
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

/* A row keeps the mode and options it was booked with beside the renderer
 * they name, which is what a savestate carries. */
UTEST(progmap, a_row_keeps_what_its_fill_was_booked_with)
{
    for (size_t i = 0; i < FILL_COUNT; i++)
    {
        vga_prog_t row = {0};
        ASSERT_TRUE(vga_prog_row_fill(&row, 1, fills[i].mode, fills[i].attr, 0x1234));
        ASSERT_TRUE(row.fill_fn[1] == vga_mode_fill_fn(fills[i].mode, fills[i].attr));
        ASSERT_EQ((int)row.fill_mode[1], (int)fills[i].mode);
        ASSERT_EQ((int)row.fill_options[1], (int)fills[i].attr);
        ASSERT_EQ((int)row.fill_config[1], 0x1234);
        ASSERT_TRUE(row.fill_fn[0] == NULL);
    }
    vga_prog_t row = {0};
    ASSERT_FALSE(vga_prog_row_fill(&row, 0, 1, 5, 0));
    ASSERT_TRUE(row.fill_fn[0] == NULL);
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

UTEST(progmap, mode_two_options_are_in_the_row)
{
    static const uint16_t options[] = {0, 1, 2, 3, 8, 9, 10, 11, 0x0128, 0x4553};
    vga_fill_fn_t fn = vga_mode_fill_fn(2, 0);
    ASSERT_TRUE(fn != NULL);
    for (unsigned i = 0; i < sizeof options / sizeof options[0]; i++)
    {
        vga_prog_t row = {0};
        ASSERT_TRUE(vga_prog_row_fill(&row, 2, 2, options[i], 0));
        ASSERT_TRUE(row.fill_fn[2] == fn);
        ASSERT_EQ((int)row.fill_mode[2], 2);
        ASSERT_EQ((int)row.fill_options[2], (int)options[i]);
    }
    /* A depth past 8bpp, and a trim that leaves none of an 8 pixel tile. */
    ASSERT_TRUE(vga_mode_fill_fn(2, 4) == NULL);
    ASSERT_TRUE(vga_mode_fill_fn(2, 0x0080) == NULL);
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

static void terminal_write(const char *text)
{
    manifold_write_to(COM_TERM_SCREEN, text, strlen(text));
}

/* The terminal builds the pairs of a row's colors for the first line it
 * draws of the row and keeps them in the caller's cache for the rest. They
 * must not outlast the row: another row, the rows changing places, DECSCNM,
 * and a cell recolored by the next frame all show. */
UTEST(progmap, a_terminal_row_keeps_no_colors_it_should_not)
{
    static uint16_t line[640];
    static vga_fill_cache_t cache;
    ASSERT_TRUE(vga_canvas_select(vga_canvas_640_480));
    ASSERT_TRUE(terminal_at(0, 0));
    const vga_fill_fn_t term = mode0_fill_fn(0);
    /* A red space on the top row and a green one on the next. The
     * terminal publishes what it wrote from its task. */
    terminal_write("\33[H\33[48;2;255;0;0m \r\n\33[48;2;0;255;0m \33[m");
    emu_frames(1);
    const uint32_t row0 = mode0_regs.row[0], row1 = mode0_regs.row[1];
    ASSERT_TRUE(term(0, 0, 640, line, 0, &cache));
    const uint16_t red = line[0];
    /* The next row, then the first again. */
    ASSERT_TRUE(term(0, 16, 640, line, 0, &cache));
    const uint16_t green = line[0];
    ASSERT_NE(red, green);
    ASSERT_TRUE(term(0, 1, 640, line, 0, &cache));
    ASSERT_EQ(line[0], red);
    /* The rows trade places part way down the first. */
    mode0_regs.row[0] = row1;
    ASSERT_TRUE(term(0, 2, 640, line, 0, &cache));
    ASSERT_EQ(line[0], green);
    mode0_regs.row[0] = row0;
    ASSERT_TRUE(term(0, 3, 640, line, 0, &cache));
    ASSERT_EQ(line[0], red);
    /* DECSCNM part way down: a space shows its foreground. */
    const uint32_t screen = mode0_regs.screen;
    mode0_regs.screen = screen | MODE0_SCREEN_REVERSE;
    ASSERT_TRUE(term(0, 4, 640, line, 0, &cache));
    ASSERT_NE(line[0], red);
    mode0_regs.screen = screen;
    ASSERT_TRUE(term(0, 5, 640, line, 0, &cache));
    ASSERT_EQ(line[0], red);
    /* The top row's cell recolored where it is. */
    terminal_write("\33[H\33[48;2;0;255;0m \33[m");
    emu_frames(1);
    ASSERT_TRUE(term(0, 6, 640, line, 0, &cache));
    ASSERT_EQ(line[0], green);
    terminal_write("\33[2J\33[H");
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
}

/* Every underline style, drawn by the terminal's renderer and checked a
 * pixel at a time against what the style's rows of pixels say: on the
 * glyph's last three rows, in the foreground's color as more of the glyph,
 * or in SGR 58's color over the glyph. On both sizes of glyph, on a
 * double-width row, and on a reversed screen, where an underline in the
 * foreground's color reverses with the glyph and one in its own color does
 * not. The last cell has a strike through it as well. */
UTEST(progmap, the_terminal_draws_every_underline_style)
{
    static uint16_t line[640];
    static vga_fill_cache_t cache;
    static const struct
    {
        vga_canvas_t canvas;
        int width, ch;
    } sizes[] = {{vga_canvas_640_480, 640, 16}, {vga_canvas_320_240, 320, 8}};
    const vga_fill_fn_t term = mode0_fill_fn(0);
    for (unsigned z = 0; z < 2; z++)
        for (int wide = 0; wide < 2; wide++)
            for (int reverse = 0; reverse < 2; reverse++)
            {
                const int ch = sizes[z].ch, last = ch - 1;
                const uint8_t *font = ch == 16 ? font16 : font8;
                const uint8_t (*table)[8] = ch == 16 ? mode0_underline16 : mode0_underline8;
                ASSERT_TRUE(vga_canvas_select(sizes[z].canvas));
                ASSERT_TRUE(terminal_at(0, 0));
                terminal_write(reverse ? "\33[?5h" : "\33[?5l");
                terminal_write("\33[m\33[2J\33[H");
                terminal_write(wide ? "\33#6" : "\33#5");
                terminal_write("\33[38;2;255;0;0;48;2;0;0;255m");
                int cells = 0;
                for (int own = 0; own < 2; own++)
                    for (int style = 0; style <= 5; style++, cells++)
                    {
                        char seq[40];
                        snprintf(seq, sizeof seq, "%s\33[4:%dmg", own ? "\33[58;2;0;255;0m" : "\33[59m", style);
                        terminal_write(seq);
                    }
                terminal_write("\33[59;9;4:3mg");
                cells++;
                emu_frames(1);
                const mode0_cell_t *cell = &mode0_cells[MODE0_ROW_CELL(mode0_regs.row[0])];
                const mode0_colors_t *colors = &mode0_colors[MODE0_ROW_CELL(mode0_regs.row[0])];
                for (int y = 0; y < ch; y++)
                {
                    ASSERT_TRUE(term(0, (int16_t)y, (int16_t)sizes[z].width, line, 0, &cache));
                    const uint8_t *underline = table[y + 3 > last ? y + 3 - last : 0];
                    for (int i = 0; i < cells; i++)
                    {
                        const uint16_t attr = cell[i].attributes;
                        const bool own = attr & MODE0_ATTR_UL_COLOR;
                        ASSERT_EQ((int)own, (int)(i >= 7 && i < 12));
                        uint8_t bits = font[y * 256 + 'g'];
                        if ((attr & TERM_ATTR_STRIKE) && y == ch / 2)
                            bits = 0xFF;
                        const uint8_t ul = underline[(attr >> TERM_ATTR_UL_SHIFT) & 7];
                        uint16_t fg = mode0_color(colors[i].fg_color);
                        uint16_t bg = mode0_color(colors[i].bg_color);
                        if (reverse)
                        {
                            const uint16_t t = fg;
                            fg = bg;
                            bg = t;
                        }
                        for (int k = 0; k < 8; k++)
                        {
                            const bool on = (bits >> (7 - k)) & 1, line_on = (ul >> (7 - k)) & 1;
                            const uint16_t want = own && line_on ? mode0_color(colors[i].ul_color)
                                                  : on || line_on ? fg
                                                                  : bg;
                            for (int d = 0; d <= wide; d++)
                                ASSERT_EQ(line[(i * 8 + k) * (wide + 1) + d], want);
                        }
                    }
                }
            }
    /* The shapes: nothing above the last three rows, a single line on the
     * last row, a double one on the last and the third from last, and
     * every style its own. */
    for (int t = 0; t < 2; t++)
    {
        const uint8_t (*table)[8] = t ? mode0_underline8 : mode0_underline16;
        for (int style = 0; style < 8; style++)
        {
            ASSERT_EQ(table[0][style], 0);
            const bool drawn = table[1][style] | table[2][style] | table[3][style];
            ASSERT_EQ((int)drawn, (int)(style >= 1 && style <= 5));
            for (int other = 0; other < style; other++)
                if (drawn)
                    ASSERT_TRUE(table[1][style] != table[1][other] || table[2][style] != table[2][other] ||
                                table[3][style] != table[3][other]);
        }
        ASSERT_EQ(table[1][TERM_UL_SINGLE] | table[2][TERM_UL_SINGLE], 0);
        ASSERT_EQ(table[3][TERM_UL_SINGLE], 0xFF);
        ASSERT_EQ(table[1][TERM_UL_DOUBLE] & table[3][TERM_UL_DOUBLE], 0xFF);
        ASSERT_EQ(table[2][TERM_UL_DOUBLE], 0);
    }
    /* The pixels themselves, as pictures: the last three rows of the
     * 16-row glyphs, then of the 8-row ones. */
    static const char *const shapes[2][6][3] = {
        {{"........", "........", "........"},
         {"........", "........", "XXXXXXXX"},
         {"XXXXXXXX", "........", "XXXXXXXX"},
         {"X.....XX", ".X...X..", "..XXX..."},
         {"........", "........", "X.X.X.X."},
         {"........", "........", ".XXXXX.."}},
        {{"........", "........", "........"},
         {"........", "........", "XXXXXXXX"},
         {"XXXXXXXX", "........", "XXXXXXXX"},
         {"........", ".XX..XX.", "X..XX..X"},
         {"........", "........", "X.X.X.X."},
         {"........", "........", ".XXXXX.."}}};
    for (int t = 0; t < 2; t++)
        for (int style = 0; style < 6; style++)
            for (int r = 0; r < 3; r++)
                for (int k = 0; k < 8; k++)
                    ASSERT_EQ((int)(((t ? mode0_underline8 : mode0_underline16)[r + 1][style] >> (7 - k)) & 1),
                              (int)(shapes[t][style][r][k] == 'X'));
    /* A block cursor on an underlined cell shows the underline through it,
     * in the cell's background, as it shows the glyph. */
    ASSERT_TRUE(vga_canvas_select(vga_canvas_640_480));
    ASSERT_TRUE(terminal_at(0, 0));
    terminal_write("\33[?5l\33[m\33#5\33[2J\33[H\33[48;2;0;0;255;4m \33[H\33[2 q");
    emu_frames(1);
    ASSERT_TRUE(term(0, 15, 640, line, 0, &cache));
    const uint16_t blue = mode0_color(mode0_colors[MODE0_ROW_CELL(mode0_regs.row[0])].bg_color);
    for (int k = 0; k < 8; k++)
        ASSERT_EQ(line[k], blue);
    ASSERT_TRUE(term(0, 14, 640, line, 0, &cache));
    for (int k = 0; k < 8; k++)
        ASSERT_NE(line[k], blue);
    /* The font's second table: an italic is not the upright glyph, and a
     * DEC Special Graphics line is a line. On the 8-row glyphs, which have
     * no italics, an italic is the upright glyph. */
    static uint16_t upright[640];
    terminal_write("\33[m\33[2J\33[HR\33[3mR\33[23m\33(0q\33(B\33[H\33[?25l");
    emu_frames(1);
    bool differs = false, drawn = false;
    for (int y = 0; y < 16; y++)
    {
        ASSERT_TRUE(term(0, (int16_t)y, 640, line, 0, &cache));
        differs |= memcmp(line, line + 8, 8 * sizeof line[0]) != 0;
        for (int k = 0; k < 8; k++)
            drawn |= line[16 + k] != line[639];
    }
    ASSERT_TRUE(differs);
    ASSERT_TRUE(drawn);
    ASSERT_TRUE(vga_canvas_select(vga_canvas_320_240));
    ASSERT_TRUE(terminal_at(0, 0));
    terminal_write("\33[m\33[2J\33[HR\33[3mR\33[23m\33(0q\33(B\33[H\33[?25l");
    emu_frames(1);
    drawn = false;
    for (int y = 0; y < 8; y++)
    {
        ASSERT_TRUE(term(0, (int16_t)y, 320, upright, 0, &cache));
        ASSERT_EQ(0, memcmp(upright, upright + 8, 8 * sizeof upright[0]));
        for (int k = 0; k < 8; k++)
            drawn |= upright[16 + k] != upright[319];
    }
    ASSERT_TRUE(drawn);
    terminal_write("\33[?25h");
    terminal_write("\33[?5l\33[m\33#5\33[2J\33[H\33[0 q");
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
}

static void put16(unsigned at, uint16_t v)
{
    xram[at] = (uint8_t)v;
    xram[at + 1] = (uint8_t)(v >> 8);
}

/* Mode 1's colored depths keep a row's pairs the same way. Another row, a
 * scroll across, and a cell recolored by the next frame all show. */
UTEST(progmap, a_mode1_row_keeps_no_colors_it_should_not)
{
    static uint16_t line[640];
    static vga_fill_cache_t cache;
    ASSERT_TRUE(vga_canvas_select(vga_canvas_640_480));
    /* 80 by 30 cells of 16-bit color at 0x1000, in the built-in 8x16 font. */
    const unsigned cfg = 0x100, cells = 0x1000;
    xram[cfg] = xram[cfg + 1] = 0;
    put16(cfg + 2, 0);
    put16(cfg + 4, 0);
    put16(cfg + 6, 80);
    put16(cfg + 8, 30);
    put16(cfg + 10, cells);
    put16(cfg + 12, 0xFFFF);
    put16(cfg + 14, 0xFFFF);
    const uint16_t red = 0x003F, green = 0x07E0, blue = 0xF820, white = 0xFFFF;
    for (unsigned i = 0; i < 80 * 30; i++)
    {
        xram[cells + 6 * i] = ' ';
        xram[cells + 6 * i + 1] = 0;
        put16(cells + 6 * i + 2, white);
        put16(cells + 6 * i + 4, i < 80 ? red : green);
    }
    put16(cells + 6 + 4, blue); /* the second cell of the top row */
    const vga_fill_fn_t text = vga_mode_fill_fn(1, 12);
    ASSERT_TRUE(text(0, 0, 640, line, cfg, &cache));
    ASSERT_EQ(line[0], red);
    ASSERT_EQ(line[8], blue);
    /* The next row, then the first again. */
    ASSERT_TRUE(text(0, 16, 640, line, cfg, &cache));
    ASSERT_EQ(line[0], green);
    ASSERT_TRUE(text(0, 1, 640, line, cfg, &cache));
    ASSERT_EQ(line[0], red);
    /* Scrolled a cell and a pixel to the left part way down the row. */
    put16(cfg + 2, (uint16_t)-9);
    ASSERT_TRUE(text(0, 2, 640, line, cfg, &cache));
    ASSERT_EQ(line[0], blue);
    ASSERT_EQ(line[6], blue);
    ASSERT_EQ(line[7], red);
    put16(cfg + 2, 0);
    ASSERT_TRUE(text(0, 3, 640, line, cfg, &cache));
    ASSERT_EQ(line[0], red);
    /* The first cell recolored where it is. */
    put16(cells + 4, green);
    emu_frames(1);
    ASSERT_TRUE(text(0, 4, 640, line, cfg, &cache));
    ASSERT_EQ(line[0], green);
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
}

static uint32_t rand_state;
static uint32_t rand_next(void)
{
    rand_state = rand_state * 1664525u + 1013904223u;
    return rand_state >> 8;
}

/* Mode 1 the plain way: every pixel of a scanline from its cell, with no
 * tables and nothing kept. False for a scanline mode 1 does not draw. */
static bool mode1_reference(uint16_t attr, int y, int width, unsigned cfg, uint16_t *out)
{
    const int font_h = (attr & 8) ? 16 : 8;
    const int depth = attr & 7;
    const int cell_size = depth == 0 ? 1 : depth <= 2 ? 2 : depth == 3 ? 3 : 6;
    const int bpp = depth == 0 ? 1 : depth <= 2 ? 4 : 8;
    const bool x_wrap = xram[cfg], y_wrap = xram[cfg + 1];
    int16_t c[7];
    memcpy(c, (const uint8_t *)&xram[cfg + 2], sizeof(c));
    const int x_pos = c[0], y_pos = c[1], wc = c[2], hc = c[3];
    const unsigned data_at = (uint16_t)c[4], pal_at = (uint16_t)c[5], font_at = (uint16_t)c[6];
    if (wc < 1 || wc > INT16_MAX / 8 || hc < 1 || hc * font_h > INT16_MAX)
        return false;
    const int height = hc * font_h, width_px = wc * 8;
    int row = y - y_pos;
    if (y_wrap)
        row = ((row % height) + height) % height;
    if (row < 0 || row >= height)
        return false;
    if (!x_wrap && (x_pos >= width || x_pos + width_px <= 0))
        return false;
    if ((unsigned)hc * wc * cell_size > 0x10000 - data_at)
        return false;
    const uint16_t *builtin = bpp == 1 ? color_2 : color_256;
    const bool own_palette = !(pal_at & 1) && pal_at <= 0x10000 - 2u * (1u << bpp);
    const bool own_font = font_at <= 0x10000 - 256u * font_h;
    const uint8_t *font = own_font ? (const uint8_t *)&xram[font_at] : font_h == 8 ? font8 : font16;
    for (int x = 0; x < width; x++)
    {
        int px = x - x_pos;
        if (x_wrap)
            px = ((px % width_px) + width_px) % width_px;
        if (px < 0 || px >= width_px)
        {
            out[x] = 0;
            continue;
        }
        const uint8_t *cell = (const uint8_t *)&xram[data_at + (row / font_h * wc + px / 8) * cell_size];
        const bool on = font[256 * (row % font_h) + cell[0]] & (0x80 >> (px % 8));
        unsigned index = 0;
        uint16_t color = 0;
        switch (depth)
        {
        case 0:
            index = on;
            break;
        case 1:
            index = on ? cell[1] >> 4 : cell[1] & 0xF;
            break;
        case 2:
            index = on ? cell[1] & 0xF : cell[1] >> 4;
            break;
        case 3:
            index = on ? cell[1] : cell[2];
            break;
        default:
            memcpy(&color, cell + (on ? 2 : 4), 2);
            break;
        }
        if (depth != 4)
        {
            if (own_palette)
                memcpy(&color, (const uint8_t *)&xram[pal_at + 2 * index], 2);
            else
                color = builtin[index];
        }
        out[x] = color;
    }
    return true;
}

/* Every depth of mode 1 draws what its cells say, on the first scanline of
 * a row, where the colored depths build the row's pairs, and on the rest,
 * where they reuse them: scrolled, wrapped, cut by either edge of the line,
 * and with its own palette and font or the built-in ones. */
UTEST(progmap, mode1_draws_what_its_cells_say)
{
    static uint16_t line[640], want[640];
    static vga_fill_cache_t cache;
    static const uint16_t attrs[] = {0, 1, 2, 3, 4, 8, 9, 10, 11, 12};
    rand_state = 12345;
    for (int n = 0; n < 20000; n++)
    {
        if (n % 64 == 0)
            for (unsigned i = 0; i < 0x10000; i++)
                xram[i] = (uint8_t)rand_next();
        const uint16_t attr = attrs[rand_next() % 10];
        const int width = (rand_next() & 1) ? 640 : 320;
        const unsigned cfg = rand_next() % 0x7FF0 * 2;
        xram[cfg] = rand_next() & 1;
        xram[cfg + 1] = rand_next() & 1;
        put16(cfg + 2, (uint16_t)((int)(rand_next() % 1300) - 650));
        put16(cfg + 4, (uint16_t)((int)(rand_next() % 1300) - 650));
        put16(cfg + 6, (uint16_t)((rand_next() & 1) ? rand_next() % 12 + 1 : rand_next() % 300 + 1));
        put16(cfg + 8, (uint16_t)(rand_next() % 40 + 1));
        put16(cfg + 10, (uint16_t)(rand_next() % 0x4000));
        put16(cfg + 12, (uint16_t)((rand_next() & 1) ? 0xFFFF : rand_next() % 0x7000 * 2));
        put16(cfg + 14, (uint16_t)((rand_next() & 1) ? 0xFFFF : rand_next() % 0xE000));
        /* What the cache holds was built from other cells and palettes. */
        vga_fill_cache_forget_row(&cache);
        const vga_fill_fn_t fill = vga_mode_fill_fn(1, attr);
        int y = (int)(rand_next() % 480);
        for (int k = 0; k < 24; k++)
        {
            memset(line, 0x5A, sizeof(line));
            const bool drew = fill(0, (int16_t)y, (int16_t)width, line, (uint16_t)cfg, &cache);
            const bool wanted = mode1_reference(attr, y, width, cfg, want);
            ASSERT_TRUE(drew == wanted);
            if (drew)
                ASSERT_TRUE(!memcmp(line, want, (size_t)width * 2));
            y = (rand_next() % 8) ? (y + 1) % 480 : (int)(rand_next() % 480);
        }
    }
}

UTEST_MAIN_EMU()
