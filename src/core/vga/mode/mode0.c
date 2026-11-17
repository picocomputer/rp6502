/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/mode/mode0.h"
#include "core/vga/mode/mode.h"
#include "core/vga/vga.h"
#include "core/term/font.h"
#include "core/term/cells.h"

/* On a word, so the console can write and move cells a word at a time. */
mode0_cell_t mode0_cells[MODE0_CELLS] __attribute__((aligned(4)));
uint8_t mode0_extras[MODE0_CELLS];
uint16_t mode0_palette[2 * MODE0_ENTRIES];
volatile mode0_regs_t mode0_regs;

#ifndef RP6502_VGA_FABRIC

#pragma GCC push_options
#pragma GCC optimize("O3")

/* The first word of the key of a row of the terminal in a plane's fill
 * cache; see vga_fill_cache_t. Mode 1's are 2 and up. */
#define MODE0_ROW_KEY 1

_Static_assert(VGA_FILL_ROW_CELLS >= 80, "a pair table for every cell of a row");

/* A double-size glyph bit covers two pixels, a word of the scanline. */
static inline __attribute__((always_inline)) void
mode0_render_wide(uint16_t *buf, uint8_t bits, const uint32_t *pair)
{
    mode_word_t *w = (mode_word_t *)buf;
    for (int k = 7; k >= 0; k--)
        *w++ = pair[((bits >> k) & 1) * 3];
}

/* The pixel pairs of every cell of a row: its colors, looked up where they
 * are entries of the palette, after DECSCNM and the blink phase, as the four
 * ways two pixels can land. */
static inline __attribute__((always_inline)) void
mode0_row_pairs(uint32_t *pair, const mode0_cell_t *cell, int cols, uint8_t blink_mask, bool rev)
{
    for (int i = 0; i < cols; i++, cell++, pair += 4)
    {
        uint16_t fg = mode0_color(cell->fg_color);
        uint16_t bg = mode0_color(cell->bg_color);
        if (rev)
        {
            uint16_t t = fg;
            fg = bg;
            bg = t;
        }
        if (cell->attributes & blink_mask)
            fg = bg;
        mode_pair_set(pair, bg, fg);
    }
}

/* One scanline of a terminal row, from the row's pixel pairs. `cols` is the
 * cells drawn and `wide` doubles every pixel across. The callers pass
 * constants, so each combination compiles to its own loop. A cell with no
 * attributes, the common one, is its glyph through its pairs. */
static inline __attribute__((always_inline)) void
mode0_draw(uint16_t *rgb, const mode0_cell_t *cell, const uint32_t *pair, int cols,
           const uint8_t *font_line, const uint8_t *font_line_dec, const uint8_t *italic_line,
           uint8_t line_mask, bool ul_row, bool wide)
{
    for (int i = 0; i < cols; i++, cell++, pair += 4, rgb += wide ? 16 : 8)
    {
        /* The glyph and the attributes in one load: with no attributes
         * the value is the glyph. */
        uint16_t ca;
        memcpy(&ca, cell, sizeof(ca));
        uint8_t bits;
        if (MODE_LIKELY(ca < 0x100))
            bits = font_line[ca];
        else
        {
            const uint8_t attr = (uint8_t)(ca >> 8);
            const uint8_t code = (uint8_t)ca;
            if (attr & line_mask)
            {
                /* A line through the cell: the foreground all the way
                 * across, or the underline's own color. */
                const uint32_t w = ul_row ? mode0_color(cell->ul_color) * 0x10001u : pair[3];
                mode_word_t *d = (mode_word_t *)rgb;
                for (int k = 0; k < (wide ? 8 : 4); k++)
                    d[k] = w;
                continue;
            }
            if (attr & TERM_ATTR_DEC)
                bits = font_line_dec[(code - 0x5F) & 31];
            else if (italic_line && (attr & TERM_ATTR_ITALIC) && code < 0x80)
                bits = italic_line[code];
            else
                bits = font_line[code];
        }
        if (wide)
            mode0_render_wide(rgb, bits, pair);
        else
            mode_render_1bpp(rgb, bits, pair);
    }
}

/* A terminal scanline: `ch` is 8 on the 320-wide canvases, where the
 * 40-column terminal is drawn, and 16 on the others. */
static inline __attribute__((always_inline)) bool
mode0_render_term(int16_t scanline_id, int16_t begin, uint16_t *rgb, const int ch,
                  const int width, const uint8_t *font, const uint8_t *font_dec,
                  const uint8_t *italic, vga_fill_cache_t *cache)
{
    /* A row past the terminal's would read past its rows, so a row copied
     * part way through a reprogram, with another mode's config, draws
     * nothing. */
    scanline_id -= begin;
    if ((uint16_t)scanline_id >= TERM_MAX_HEIGHT * ch)
        return false;
    const uint8_t logical_row = (uint8_t)(scanline_id / ch);
    const uint8_t scanrow = (uint8_t)(scanline_id % ch);
    const uint32_t row_word = mode0_regs.row[logical_row];
    const uint32_t cursor = mode0_regs.cursor;
    const uint32_t screen = mode0_regs.screen;
    const bool reverse = screen & MODE0_SCREEN_REVERSE;
    const uint8_t size = MODE0_ROW_SIZE(row_word);
    /* The glyph row this scanline shows: a double-height half draws each
     * glyph row of its half twice. */
    uint8_t glyph_row = scanrow;
    if (size == TERM_LINE_TOP)
        glyph_row = (uint8_t)(scanrow / 2);
    else if (size == TERM_LINE_BOTTOM)
        glyph_row = (uint8_t)(ch / 2 + scanrow / 2);
    const uint8_t last = (uint8_t)(ch - 1);
    const uint8_t *font_line = &font[glyph_row * 256];
    const uint8_t *font_line_dec = &font_dec[glyph_row * 32];
    const uint8_t *italic_line = italic ? &italic[glyph_row * 128] : NULL;
    const uint8_t blink_mask = MODE0_SCREEN_BLINK(screen);
    const uint8_t line_mask =
        (uint8_t)((glyph_row == last ? TERM_ATTR_UNDERLINE : 0) |
                  ((glyph_row == last || glyph_row == last - 2) ? TERM_ATTR_DBL_UL : 0) |
                  (glyph_row == ch / 2 ? TERM_ATTR_STRIKE : 0) |
                  (glyph_row == 0 ? TERM_ATTR_OVERLINE : 0));
    const bool ul_row = (line_mask & (TERM_ATTR_UNDERLINE | TERM_ATTR_DBL_UL)) != 0;
    const mode0_cell_t *cell = &mode0_cells[MODE0_ROW_CELL(row_word)];
    const bool wide = size != TERM_LINE_NORMAL;
    const int cols = wide ? width / 2 : width;
    /* The row's pairs are built for the first of its scanlines a thread
     * draws in a frame, and again when the row or the screen's look, which
     * holds the blink phase and DECSCNM, changes part way down it. */
    const uint32_t key[4] = {MODE0_ROW_KEY, row_word, screen, (uint32_t)vga_frame_count()};
    if (memcmp(cache->row_key, key, sizeof(key)))
    {
        mode0_row_pairs(cache->row_pair[0], cell, cols, blink_mask, reverse);
        memcpy(cache->row_key, key, sizeof(key));
    }
    const uint32_t *pair = cache->row_pair[0];
    if (wide)
        mode0_draw(rgb, cell, pair, width / 2, font_line, font_line_dec, italic_line,
                   line_mask, ul_row, true);
    else
        mode0_draw(rgb, cell, pair, width, font_line, font_line_dec, italic_line,
                   line_mask, ul_row, false);
    // DECSCUSR styles 2, 4 and 6 are the steady ones, so they draw whether or
    // not cursor_lit is set: cells.c toggles cursor_lit on a timer and leaves the
    // style to the renderer.
    const uint8_t cursor_style = MODE0_CURSOR_STYLE(cursor);
    if (logical_row == MODE0_CURSOR_Y(cursor) &&
        (cursor & MODE0_CURSOR_ENABLED) &&
        ((cursor & MODE0_CURSOR_LIT) ||
         cursor_style == 2 ||
         cursor_style == 4 ||
         cursor_style == 6))
    {
        uint8_t cx = MODE0_CURSOR_X(cursor);
        // A deferred wrap parks the cursor one column past the last, so cx can
        // be the row's width. It is drawn on the last column instead, as a
        // block (DECSCUSR style 1) whatever style is set.
        bool wrap_pending = (cx >= cols);
        if (wrap_pending)
            cx = (uint8_t)(cols - 1);
        uint16_t *crgb = rgb + (uint32_t)cx * (wide ? 16 : 8);
        const uint16_t cursor_color = (uint16_t)mode0_regs.cursor_color;
        MODE_TABLE(cpair, 4);
        switch (wrap_pending ? 1u : cursor_style)
        {
        case 3:
        case 4: // underline
            if (scanrow == last || (ch == 16 && scanrow == last - 1))
            {
                mode_pair_set(cpair, cursor_color, cursor_color);
                mode_render_1bpp(crgb, 0xFF, cpair);
                if (wide)
                    mode_render_1bpp(crgb + 8, 0xFF, cpair);
            }
            break;
        case 5:
        case 6: // bar
            crgb[0] = cursor_color;
            if (ch == 16)
                crgb[1] = cursor_color;
            break;
        default:
        { // 0, 1, 2: block
            const mode0_cell_t *cp = cell + cx;
            uint8_t cattr = cp->attributes;
            uint8_t cbits = font_line[cp->font_code];
            if (cattr & TERM_ATTR_DEC)
                cbits = font_line_dec[(cp->font_code - 0x5F) & 31];
            else if (italic_line && (cattr & TERM_ATTR_ITALIC) && cp->font_code < 0x80)
                cbits = italic_line[cp->font_code];
            if (cattr & line_mask)
                cbits = 0xFF;
            mode_pair_set(cpair, cursor_color,
                          mode0_color(reverse ? cp->fg_color : cp->bg_color));
            if (wide)
                mode0_render_wide(crgb, cbits, cpair);
            else
                mode_render_1bpp(crgb, cbits, cpair);
            break;
        }
        }
    }
    return true;
}

/* The config of a terminal row is the scanline the terminal begins at, so
 * each row says where it is in the terminal. A render on the other core
 * copies the row whole, and a begin kept apart from the rows could pair an
 * old row with a new begin, or a new row with an old one. */
static bool
mode0_render(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr,
             vga_fill_cache_t *cache)
{
    (void)plane_id;
    if (width == 320)
        return mode0_render_term(scanline_id, (int16_t)config_ptr, rgb, 8, 40,
                                 font8, font_dec_8, NULL, cache);
    else
        return mode0_render_term(scanline_id, (int16_t)config_ptr, rgb, 16, 80,
                                 font16, font_dec_16, italic16, cache);
}
#pragma GCC pop_options

vga_fill_fn_t mode0_fill_fn(uint16_t attributes)
{
    return attributes ? NULL : mode0_render;
}

bool mode0_fill_attr(vga_fill_fn_t fn, uint16_t *attributes)
{
    if (fn != mode0_render)
        return false;
    *attributes = 0;
    return true;
}

#else

vga_fill_fn_t mode0_fill_fn(uint16_t attributes)
{
    (void)attributes;
    return NULL;
}

#endif

bool mode0_prog(uint16_t *xregs)
{
    int16_t plane = (int16_t)xregs[2];
    int16_t scanline_begin, scanline_end;
    uint8_t cols, rows;
    if (!mode0_place(vga_canvas_width(), vga_canvas_height(), xregs,
                     &scanline_begin, &scanline_end, &cols, &rows))
        return false;

    if (vga_prog_exclusive(plane, scanline_begin, scanline_end,
                           (uint16_t)scanline_begin, mode0_fill_fn(0)))
    {
        cells_set_height(cols, rows);
        return true;
    }
    return false;
}
