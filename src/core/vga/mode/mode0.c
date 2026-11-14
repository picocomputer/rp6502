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

#pragma GCC push_options
#pragma GCC optimize("O3")

/* A double-size glyph bit covers two pixels, a word of the scanline. */
static inline __attribute__((always_inline)) void
mode0_render_wide(uint16_t *buf, uint8_t bits, const uint32_t *pair)
{
    mode_word_t *w = (mode_word_t *)buf;
    for (int k = 7; k >= 0; k--)
        *w++ = pair[((bits >> k) & 1) * 3];
}

/* One scanline of a terminal row. `ch` is the cell height, 8 or 16, `cols`
 * the cells drawn, `wide` doubles every pixel across, and `rev` swaps every
 * cell's colors for DECSCNM. The callers pass constants, so each combination
 * compiles to its own loop. */
static inline __attribute__((always_inline)) void
mode0_cells(uint16_t *rgb, const term_data_t *cell, int cols, const uint8_t *font_line,
            const uint8_t *font_line_dec, const uint8_t *italic_line,
            uint8_t blink_mask, uint8_t line_mask, bool ul_row, bool wide, bool rev)
{
    MODE_TABLE(pair, 4);
    for (int i = 0; i < cols; i++, cell++)
    {
        uint8_t attr = cell->attributes;
        uint8_t bits = font_line[cell->font_code];
        uint16_t fg = cell->fg_color;
        uint16_t bg = cell->bg_color;
        if (rev)
        {
            uint16_t t = fg;
            fg = bg;
            bg = t;
        }
        if (attr)
        {
            if (attr & TERM_ATTR_DEC)
                bits = font_line_dec[(cell->font_code - 0x5F) & 31];
            else if (italic_line && (attr & TERM_ATTR_ITALIC) && cell->font_code < 0x80)
                bits = italic_line[cell->font_code];
            if (attr & blink_mask)
                fg = bg;
            if (attr & line_mask)
            {
                bits = 0xFF;
                if (ul_row)
                    fg = cell->ul_color;
            }
        }
        mode_pair_set(pair, bg, fg);
        if (wide)
        {
            mode0_render_wide(rgb, bits, pair);
            rgb += 16;
        }
        else
        {
            mode_render_1bpp(rgb, bits, pair);
            rgb += 8;
        }
    }
}

/* A terminal scanline: `ch` is 8 on the 320-wide canvases, where the
 * 40-column terminal is drawn, and 16 on the others. */
static inline __attribute__((always_inline)) bool
mode0_render_term(int16_t scanline_id, int16_t begin, uint16_t *rgb, const int ch,
                  const int width, const uint8_t *font, const uint8_t *font_dec,
                  const uint8_t *italic)
{
    /* A row past the terminal's would read past its rows, so a row copied
     * part way through a reprogram, with another mode's config, draws
     * nothing. */
    scanline_id -= begin;
    if ((uint16_t)scanline_id >= TERM_MAX_HEIGHT * ch)
        return false;
    cells_view_t tv;
    cells_view(&tv);
    const uint8_t logical_row = (uint8_t)(scanline_id / ch);
    const uint8_t scanrow = (uint8_t)(scanline_id % ch);
    const uint8_t size = cells_view_row_size(logical_row);
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
    const uint8_t blink_mask = tv.blink_phase;
    const uint8_t line_mask =
        (uint8_t)((glyph_row == last ? TERM_ATTR_UNDERLINE : 0) |
                  ((glyph_row == last || glyph_row == last - 2) ? TERM_ATTR_DBL_UL : 0) |
                  (glyph_row == ch / 2 ? TERM_ATTR_STRIKE : 0) |
                  (glyph_row == 0 ? TERM_ATTR_OVERLINE : 0));
    const bool ul_row = (line_mask & (TERM_ATTR_UNDERLINE | TERM_ATTR_DBL_UL)) != 0;
    const term_data_t *cell = cells_view_row(logical_row);
    const bool wide = size != TERM_LINE_NORMAL;
    const int cols = wide ? width / 2 : width;
    if (wide)
    {
        if (tv.reverse)
            mode0_cells(rgb, cell, width / 2, font_line, font_line_dec, italic_line,
                        blink_mask, line_mask, ul_row, true, true);
        else
            mode0_cells(rgb, cell, width / 2, font_line, font_line_dec, italic_line,
                        blink_mask, line_mask, ul_row, true, false);
    }
    else if (tv.reverse)
        mode0_cells(rgb, cell, width, font_line, font_line_dec, italic_line,
                    blink_mask, line_mask, ul_row, false, true);
    else
        mode0_cells(rgb, cell, width, font_line, font_line_dec, italic_line,
                    blink_mask, line_mask, ul_row, false, false);
    // DECSCUSR styles 2, 4 and 6 are the steady ones, so they draw whether or
    // not cursor_lit is set: cells.c toggles cursor_lit on a timer and leaves the
    // style to the renderer.
    if (logical_row == tv.cursor_y &&
        tv.cursor_enabled &&
        (tv.cursor_lit ||
         tv.cursor_style == 2 ||
         tv.cursor_style == 4 ||
         tv.cursor_style == 6))
    {
        uint8_t cx = tv.cursor_x;
        // A deferred wrap parks the cursor one column past the last, so cx can
        // be the row's width. It is drawn on the last column instead, as a
        // block (DECSCUSR style 1) whatever style is set.
        bool wrap_pending = (cx >= cols);
        if (wrap_pending)
            cx = (uint8_t)(cols - 1);
        uint16_t *crgb = rgb + (uint32_t)cx * (wide ? 16 : 8);
        const uint16_t cursor_color = tv.cursor_color;
        MODE_TABLE(cpair, 4);
        switch (wrap_pending ? 1u : tv.cursor_style)
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
            const term_data_t *cp = cell + cx;
            uint8_t cattr = cp->attributes;
            uint8_t cbits = font_line[cp->font_code];
            if (cattr & TERM_ATTR_DEC)
                cbits = font_line_dec[(cp->font_code - 0x5F) & 31];
            else if (italic_line && (cattr & TERM_ATTR_ITALIC) && cp->font_code < 0x80)
                cbits = italic_line[cp->font_code];
            if (cattr & line_mask)
                cbits = 0xFF;
            mode_pair_set(cpair, cursor_color, tv.reverse ? cp->fg_color : cp->bg_color);
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
    (void)cache;
    if (width == 320)
        return mode0_render_term(scanline_id, (int16_t)config_ptr, rgb, 8, 40,
                                 font8, font_dec_8, NULL);
    else
        return mode0_render_term(scanline_id, (int16_t)config_ptr, rgb, 16, 80,
                                 font16, font_dec_16, italic16);
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

bool mode0_prog(uint16_t *xregs)
{
    int16_t plane = xregs[2];
    int16_t scanline_begin = xregs[3];
    int16_t scanline_end = xregs[4];
    uint8_t cols, rows;
    if (!mode0_layout(vga_canvas_width(), vga_canvas_height(),
                      &scanline_begin, &scanline_end, &cols, &rows))
        return false;

    if (vga_prog_exclusive(plane, scanline_begin, scanline_end,
                           (uint16_t)scanline_begin, mode0_render))
    {
        cells_set_height(cols, rows);
        return true;
    }
    return false;
}
