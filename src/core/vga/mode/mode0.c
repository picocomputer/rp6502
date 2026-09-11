/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/mode/mode0.h"
#include "core/vga/mode/mode.h"
#include "core/vga/vga.h"
#include "core/term/font.h"
#include "core/term/term.h"

static int16_t mode0_scanline_begin;

#pragma GCC push_options
#pragma GCC optimize("O3")
static inline bool
mode0_render_320(int16_t scanline_id, uint16_t *rgb)
{
    scanline_id -= mode0_scanline_begin;
    term_view_t tv;
    term_view(&tv);
    const uint8_t scanrow = (uint8_t)(scanline_id & 7);
    const uint8_t *font_line = &font8[scanrow * 256];
    /* The DEC graphics font holds 32 glyphs per scan row, one for each of the
     * codes 0x5F through 0x7E that term_out_glyph marks TERM_ATTR_DEC. */
    const uint8_t *font_line_dec = &font_dec_8[scanrow * 32];
    const uint8_t blink_mask = tv.blink_phase;
    const uint8_t line_mask =
        (uint8_t)((scanrow == 7 ? TERM_ATTR_UNDERLINE : 0) |
                  ((scanrow == 7 || scanrow == 5) ? TERM_ATTR_DBL_UL : 0) |
                  (scanrow == 4 ? TERM_ATTR_STRIKE : 0) |
                  (scanrow == 0 ? TERM_ATTR_OVERLINE : 0));
    const bool ul_row = (line_mask & (TERM_ATTR_UNDERLINE | TERM_ATTR_DBL_UL)) != 0;
    const uint8_t logical_row = (uint8_t)(scanline_id / 8);
    const term_data_t *cell = term_view_row(logical_row);
    uint16_t *const rgb_line = rgb;
    for (int i = 0; i < 40; i++, cell++)
    {
        uint8_t attr = cell->attributes;
        uint8_t bits = font_line[cell->font_code];
        uint16_t fg = cell->fg_color;
        uint16_t bg = cell->bg_color;
        if (attr)
        {
            if (attr & TERM_ATTR_DEC)
                bits = font_line_dec[(cell->font_code - 0x5F) & 31];
            if (attr & blink_mask)
                fg = bg;
            if (attr & line_mask)
            {
                bits = 0xFF;
                if (ul_row)
                    fg = cell->ul_color;
            }
        }
        mode_render_1bpp(rgb, bits, bg, fg);
        rgb += 8;
    }
    // DECSCUSR styles 2, 4 and 6 are the steady ones, so they draw whether or
    // not cursor_lit is set: term.c toggles cursor_lit on a timer and leaves the
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
        // be the terminal's width. It is drawn on the last column instead, as a
        // block (DECSCUSR style 1) whatever style is set.
        bool wrap_pending = (cx >= 40);
        if (wrap_pending)
            cx = (uint8_t)(40 - 1);
        uint16_t *crgb = rgb_line + (uint32_t)cx * 8;
        const uint16_t cursor_color = tv.cursor_color;
        switch (wrap_pending ? 1u : tv.cursor_style)
        {
        case 3:
        case 4: // underline
            if (scanrow == 7)
                mode_render_1bpp(crgb, 0xFF, cursor_color, cursor_color);
            break;
        case 5:
        case 6: // bar
            crgb[0] = cursor_color;
            break;
        default:
        { // 0, 1, 2: block
            const term_data_t *cp = term_view_row(logical_row) + cx;
            uint8_t cattr = cp->attributes;
            uint8_t cbits = font_line[cp->font_code];
            if (cattr & TERM_ATTR_DEC)
                cbits = font_line_dec[(cp->font_code - 0x5F) & 31];
            if (cattr & line_mask)
                cbits = 0xFF;
            mode_render_1bpp(crgb, cbits, cursor_color, cp->bg_color);
            break;
        }
        }
    }
    return true;
}

static inline bool
mode0_render_640(int16_t scanline_id, uint16_t *rgb)
{
    scanline_id -= mode0_scanline_begin;
    term_view_t tv;
    term_view(&tv);
    const uint8_t scanrow = (uint8_t)(scanline_id & 15);
    const uint8_t *font_line = &font16[scanrow * 256];
    const uint8_t *font_line_dec = &font_dec_16[scanrow * 32];
    const uint8_t *italic_line = &italic16[scanrow * 128];
    const uint8_t blink_mask = tv.blink_phase;
    const uint8_t line_mask =
        (uint8_t)((scanrow == 15 ? TERM_ATTR_UNDERLINE : 0) |
                  ((scanrow == 15 || scanrow == 13) ? TERM_ATTR_DBL_UL : 0) |
                  (scanrow == 8 ? TERM_ATTR_STRIKE : 0) |
                  (scanrow == 0 ? TERM_ATTR_OVERLINE : 0));
    const bool ul_row = (line_mask & (TERM_ATTR_UNDERLINE | TERM_ATTR_DBL_UL)) != 0;
    const uint8_t logical_row = (uint8_t)(scanline_id / 16);
    const term_data_t *cell = term_view_row(logical_row);
    uint16_t *const rgb_line = rgb;
    for (int i = 0; i < 80; i++, cell++)
    {
        uint8_t attr = cell->attributes;
        uint8_t bits = font_line[cell->font_code];
        uint16_t fg = cell->fg_color;
        uint16_t bg = cell->bg_color;
        if (attr)
        {
            if (attr & TERM_ATTR_DEC)
                bits = font_line_dec[(cell->font_code - 0x5F) & 31];
            else if ((attr & TERM_ATTR_ITALIC) && cell->font_code < 0x80)
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
        mode_render_1bpp(rgb, bits, bg, fg);
        rgb += 8;
    }
    // DECSCUSR styles 2, 4 and 6 are the steady ones, so they draw whether or
    // not cursor_lit is set: term.c toggles cursor_lit on a timer and leaves the
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
        // be the terminal's width. It is drawn on the last column instead, as a
        // block (DECSCUSR style 1) whatever style is set.
        bool wrap_pending = (cx >= 80);
        if (wrap_pending)
            cx = (uint8_t)(80 - 1);
        uint16_t *crgb = rgb_line + (uint32_t)cx * 8;
        const uint16_t cursor_color = tv.cursor_color;
        switch (wrap_pending ? 1u : tv.cursor_style)
        {
        case 3:
        case 4: // underline
            if (scanrow == 14 || scanrow == 15)
                mode_render_1bpp(crgb, 0xFF, cursor_color, cursor_color);
            break;
        case 5:
        case 6: // bar
            crgb[0] = cursor_color;
            crgb[1] = cursor_color;
            break;
        default:
        { // 0, 1, 2: block
            const term_data_t *cp = term_view_row(logical_row) + cx;
            uint8_t cattr = cp->attributes;
            uint8_t cbits = font_line[cp->font_code];
            if (cattr & TERM_ATTR_DEC)
                cbits = font_line_dec[(cp->font_code - 0x5F) & 31];
            else if ((cattr & TERM_ATTR_ITALIC) && cp->font_code < 0x80)
                cbits = italic_line[cp->font_code];
            if (cattr & line_mask)
                cbits = 0xFF;
            mode_render_1bpp(crgb, cbits, cursor_color, cp->bg_color);
            break;
        }
        }
    }
    return true;
}

static bool
mode0_render(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    (void)plane_id;
    (void)config_ptr;
    if (width == 320)
        return mode0_render_320(scanline_id, rgb);
    else
        return mode0_render_640(scanline_id, rgb);
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

int16_t mode0_begin(void)
{
    return mode0_scanline_begin;
}

void mode0_set_begin(int16_t at)
{
    mode0_scanline_begin = at;
}

bool mode0_prog(uint16_t *xregs)
{
    int16_t plane = xregs[2];
    int16_t scanline_begin = xregs[3];
    int16_t scanline_end = xregs[4];
    int16_t height = vga_canvas_height();
    if (!scanline_begin && !scanline_end)
    {
        // Neither widescreen height is a multiple of the font height it uses,
        // so the default terminal is the tallest one that fits, centered: 22
        // rows either way, leaving two blank scanlines above and below at 180
        // and four at 360.
        if (height == 180)
            scanline_begin = 2, scanline_end = 178;
        if (height == 360)
            scanline_begin = 4, scanline_end = 356;
    }
    if (!scanline_end)
        scanline_end = height;
    int16_t scanline_count = scanline_end - scanline_begin;
    bool use_40 = height == 180 || height == 240;

    if (!scanline_count || scanline_count % (use_40 ? 8 : 16))
        return false;

    if (vga_prog_exclusive(plane, scanline_begin, scanline_end, 0, mode0_render))
    {
        if (use_40)
            term_set_height(40, (uint8_t)(scanline_count / 8));
        else
            term_set_height(80, (uint8_t)(scanline_count / 16));
        mode0_scanline_begin = scanline_begin;
        return true;
    }
    return false;
}
