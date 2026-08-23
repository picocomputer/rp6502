/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 0: the terminal. The model — cells, cursor, scroll remap, the ANSI
 * engine — lives in core/term/term.c; this is the scanvideo view over it,
 * reading the visible terminal through term_view and term_view_row. Hosts
 * with their own scanout hardware render from the same model and never
 * compile this file.
 */

#include "core/vga/mode0.h"
#include "core/vga/modes.h"
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
    const uint8_t *font_line_dec = &font_dec_8[scanrow * 32];
    // Each line attribute lights up only on the scan rows where its stroke
    // appears in the cell. The renderer's inner branch ANDs the cell's attr
    // with line_mask; a hit forces bits = 0xFF (a solid horizontal stroke
    // at this scan row). 8x8 cell layout:
    //   row 0     = overline
    //   row 4     = strikethrough (middle)
    //   row 5,7   = double underline
    //   row 7     = underline
    // blink_mask is the visible terminal's 2-bit cell-blink phase: bit 0 (TERM_ATTR_BLINK_FAST)
    // is the rapid off-half, bit 1 (TERM_ATTR_BLINK) the slow off-half. Each cell's
    // single blink bit ANDs against it, so the two rates pulse independently.
    const uint8_t blink_mask = tv.blink_phase;
    const uint8_t line_mask =
        (uint8_t)((scanrow == 7 ? TERM_ATTR_UNDERLINE : 0) |
                  ((scanrow == 7 || scanrow == 5) ? TERM_ATTR_DBL_UL : 0) |
                  (scanrow == 4 ? TERM_ATTR_STRIKE : 0) |
                  (scanrow == 0 ? TERM_ATTR_OVERLINE : 0));
    // SGR 58 underline color applies only on underline scanrows; hoists
    // out of the inner loop. TERM_ATTR_STRIKE and TERM_ATTR_OVERLINE always use fg.
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
                bits = font_line_dec[(uint8_t)(cell->font_code - 0x5F)];
            if (attr & blink_mask)
                fg = bg;
            if (attr & line_mask)
            {
                bits = 0xFF;
                if (ul_row)
                    fg = cell->ul_color;
            }
        }
        modes_render_1bpp(rgb, bits, bg, fg);
        rgb += 8;
    }
    // Cursor overlay: at most one cell per scanline. Patches the rendered
    // pixels in place; cursor wins over TERM_ATTR_BLINK on its cell. Steady
    // styles (2/4/6) draw regardless of cursor_lit -- blink_cursor only
    // owns the timing, the style decides whether the off half is visible.
    if (logical_row == tv.cursor_y &&
        tv.cursor_enabled &&
        (tv.cursor_lit ||
         tv.cursor_style == 2 ||
         tv.cursor_style == 4 ||
         tv.cursor_style == 6))
    {
        uint8_t cx = tv.cursor_x;
        // Wrap-pending: cursor is parked past the rightmost cell. Always
        // render the full block here regardless of cursor_style — an
        // underline strip or 1px bar at width-1 is too easy to miss for
        // a state the fast blink already flags as "different."
        bool wrap_pending = (cx >= 40);
        if (wrap_pending)
            cx = (uint8_t)(40 - 1);
        uint16_t *crgb = rgb_line + (uint32_t)cx * 8;
        const uint16_t cursor_color = tv.cursor_color;
        switch (wrap_pending ? 1u : tv.cursor_style)
        {
        case 3:
        case 4: // underline: solid strip at scanrow 7 only
            if (scanrow == 7)
                modes_render_1bpp(crgb, 0xFF, cursor_color, cursor_color);
            break;
        case 5:
        case 6: // bar: 1px at left edge on 8x8
            crgb[0] = cursor_color;
            break;
        default:
        { // 0/1/2 -- block: invert cell with cursor color
            const term_data_t *cp = term_view_row(logical_row) + cx;
            uint8_t cattr = cp->attributes;
            uint8_t cbits = font_line[cp->font_code];
            if (cattr & TERM_ATTR_DEC)
                cbits = font_line_dec[(uint8_t)(cp->font_code - 0x5F)];
            if (cattr & line_mask)
                cbits = 0xFF;
            modes_render_1bpp(crgb, cbits, cursor_color, cp->bg_color);
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
    // 8x16 cell layout:
    //   row 0      = overline
    //   row 8      = strikethrough (middle)
    //   row 13,15  = double underline
    //   row 15     = underline
    // 2-bit cell-blink phase (bit 0 = rapid off-half, bit 1 = slow off-half).
    const uint8_t blink_mask = tv.blink_phase;
    const uint8_t line_mask =
        (uint8_t)((scanrow == 15 ? TERM_ATTR_UNDERLINE : 0) |
                  ((scanrow == 15 || scanrow == 13) ? TERM_ATTR_DBL_UL : 0) |
                  (scanrow == 8 ? TERM_ATTR_STRIKE : 0) |
                  (scanrow == 0 ? TERM_ATTR_OVERLINE : 0));
    // SGR 58 underline color applies only on underline scanrows; hoists
    // out of the inner loop. TERM_ATTR_STRIKE and TERM_ATTR_OVERLINE always use fg.
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
                bits = font_line_dec[(uint8_t)(cell->font_code - 0x5F)];
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
        modes_render_1bpp(rgb, bits, bg, fg);
        rgb += 8;
    }
    // Cursor overlay: at most one cell per scanline. Underline strip is the
    // bottom 2 rows on 8x16; bar is 2px wide for proportionality. Steady
    // styles (2/4/6) draw regardless of cursor_lit -- blink_cursor only
    // owns the timing, the style decides whether the off half is visible.
    if (logical_row == tv.cursor_y &&
        tv.cursor_enabled &&
        (tv.cursor_lit ||
         tv.cursor_style == 2 ||
         tv.cursor_style == 4 ||
         tv.cursor_style == 6))
    {
        uint8_t cx = tv.cursor_x;
        // Wrap-pending: cursor is parked past the rightmost cell. Always
        // render the full block here regardless of cursor_style — an
        // underline strip or 2px bar at width-1 is too easy to miss for
        // a state the fast blink already flags as "different."
        bool wrap_pending = (cx >= 80);
        if (wrap_pending)
            cx = (uint8_t)(80 - 1);
        uint16_t *crgb = rgb_line + (uint32_t)cx * 8;
        const uint16_t cursor_color = tv.cursor_color;
        switch (wrap_pending ? 1u : tv.cursor_style)
        {
        case 3:
        case 4: // underline: solid strip at scanrows 14-15
            if (scanrow == 14 || scanrow == 15)
                modes_render_1bpp(crgb, 0xFF, cursor_color, cursor_color);
            break;
        case 5:
        case 6: // bar: 2px at left edge on 8x16
            crgb[0] = cursor_color;
            crgb[1] = cursor_color;
            break;
        default:
        { // 0/1/2 -- block: invert cell with cursor color
            const term_data_t *cp = term_view_row(logical_row) + cx;
            uint8_t cattr = cp->attributes;
            uint8_t cbits = font_line[cp->font_code];
            if (cattr & TERM_ATTR_DEC)
                cbits = font_line_dec[(uint8_t)(cp->font_code - 0x5F)];
            else if ((cattr & TERM_ATTR_ITALIC) && cp->font_code < 0x80)
                cbits = italic_line[cp->font_code];
            if (cattr & line_mask)
                cbits = 0xFF;
            modes_render_1bpp(crgb, cbits, cursor_color, cp->bg_color);
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

bool mode0_prog(uint16_t *xregs)
{
    int16_t plane = xregs[2];
    int16_t scanline_begin = xregs[3];
    int16_t scanline_end = xregs[4];
    int16_t height = vga_canvas_height();
    if (!scanline_begin && !scanline_end)
    {
        // Special case to make defaults work with widescreen
        if (height == 180)
            scanline_begin = 2, scanline_end = 178;
        if (height == 360)
            scanline_begin = 4, scanline_end = 356;
    }
    if (!scanline_end)
        scanline_end = height;
    int16_t scanline_count = scanline_end - scanline_begin;
    bool use_40 = height == 180 || height == 240;

    // Check for terminal height is multiple of font height
    if (!scanline_count || scanline_count % (use_40 ? 8 : 16))
        return false;

    // Program the new scanlines
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
