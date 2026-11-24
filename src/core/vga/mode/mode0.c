/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/mode/mode0.h"
#include "core/vga/mode/mode.h"
#include "core/vga/vga.h"
#include "core/term/font.h"
#include "core/term/vterm.h"

/* On a word, so the console can write and move cells a word at a time. */
mode0_cell_t mode0_cells[MODE0_CELLS] __attribute__((aligned(4)));
mode0_colors_t mode0_colors[MODE0_CELLS] __attribute__((aligned(4)));
uint16_t mode0_faint_palette[VTERM_PALETTE];
uint16_t mode0_faint_defaults[MODE0_DEFAULTS];
volatile mode0_regs_t mode0_regs;

static void mode0_sst_put_cells(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put(c, mode0_cells, sizeof mode0_cells);
    sst_put(c, mode0_colors, sizeof mode0_colors);
}

static bool mode0_sst_get_cells(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_get(c, mode0_cells, sizeof mode0_cells);
    sst_get(c, mode0_colors, sizeof mode0_colors);
    return sst_ok(c);
}

/* A glyph indexes the font of its size, and a color the palette. */
static bool mode0_sst_cells_ok(sst_cursor_t *c)
{
    for (unsigned i = 0; i < MODE0_CELLS; i++)
        if (!mode0_glyph_ok(mode0_cells[i].glyph, i < MODE0_CELLS_40 ? 8 : 16) ||
            !mode0_color_ok(mode0_colors[i].fg_color) ||
            !mode0_color_ok(mode0_colors[i].bg_color) ||
            !mode0_color_ok(mode0_colors[i].ul_color))
            return sst_refuse(c, "a cell outside the font or the palette");
    return true;
}

#define MODE0_SST(X)                                                   \
    X(FN, mode0_sst_put_cells, mode0_sst_get_cells,                    \
      sizeof mode0_cells + sizeof mode0_colors, 4)                     \
    X(IN_FN, mode0_cells)                                              \
    X(IN_FN, mode0_colors)                                             \
    X(VALIDATE, mode0_sst_cells_ok)                                    \
    X(SKIP, mode0_faint_palette, "draw.c makes it from the console's " \
                                 "palette after a load")               \
    X(SKIP, mode0_faint_defaults, "draw.c makes them from the "        \
                                  "console's after a load")            \
    X(SKIP, mode0_regs, "published again by the canvas's load")
SST_TABLE(mode0, MODE0_SST)

/* Indexed by row, then by style: none, single, double, curly, dotted,
 * dashed. The curly underline is a wave three rows tall and a cell wide on
 * the 16-row glyphs, its crest and its trough three pixels each, with the
 * trough under the middle of the glyph so it passes below a descender. On
 * the 8-row glyphs it is two rows tall and half a cell wide. A dash is five pixels, a pixel in from the left of its cell and
 * two from the right. */
const uint8_t mode0_underline16[4][8] = {
    {0},
    {0, 0x00, 0xFF, 0x83, 0x00, 0x00},
    {0, 0x00, 0x00, 0x44, 0x00, 0x00},
    {0, 0xFF, 0xFF, 0x38, 0xAA, 0x7C},
};
const uint8_t mode0_underline8[4][8] = {
    {0},
    {0, 0x00, 0xFF, 0x00, 0x00, 0x00},
    {0, 0x00, 0x00, 0x66, 0x00, 0x00},
    {0, 0xFF, 0xFF, 0x99, 0xAA, 0x7C},
};

/* Out of line, because the renderer inlines mode0_color in every one of its
 * loops and the faint colors are the rarer. */
__attribute__((noinline)) uint16_t mode0_color_rest(unsigned code)
{
    if (!(code & MODE0_CODE_DEFAULT))
        return mode0_faint_palette[code & 0xFF];
    return mode0_faint_defaults[code & 1];
}

#ifndef RP6502_VGA_FABRIC

#pragma GCC push_options
#pragma GCC optimize("O3")

_Static_assert(VGA_FILL_ROW_CELLS >= 80, "a pair table for every cell of a row");
_Static_assert(MODE0_GLYPH_OTHER(8) == FONT_OTHER(8) && MODE0_GLYPH_OTHER(16) == FONT_OTHER(16),
               "a glyph past the code page's is in the font's second table");

/* A double-size glyph bit covers two pixels, a word of the scanline. */
static inline __attribute__((always_inline)) void
mode0_render_wide(uint16_t *buf, uint8_t bits, const uint32_t *pair)
{
    mode_word_t *w = (mode_word_t *)buf;
    for (int k = 7; k >= 0; k--)
        *w++ = pair[((bits >> k) & 1) * 3];
}

/* How far apart two pixels are: the sum of how far apart their channels
 * are. */
static inline __attribute__((always_inline)) unsigned mode0_distance(uint16_t a, uint16_t b)
{
    const int r = (int)SCANVIDEO_R5_FROM_PIXEL(a) - (int)SCANVIDEO_R5_FROM_PIXEL(b);
    const int g = (int)SCANVIDEO_G5_FROM_PIXEL(a) - (int)SCANVIDEO_G5_FROM_PIXEL(b);
    const int u = (int)SCANVIDEO_B5_FROM_PIXEL(a) - (int)SCANVIDEO_B5_FROM_PIXEL(b);
    return (unsigned)((r < 0 ? -r : r) + (g < 0 ? -g : g) + (u < 0 ? -u : u));
}

/* The ink, in bits 15:0, and the paper, in bits 31:16, of a cell of ink f
 * and paper b that the selection takes (sel) or that is reversed
 * (inverse), or both, as xterm's updatedXtermGC draws them with
 * highlightColorMode and highlightReverse on, with highlight and
 * highlight_text the selection's colors as published (mode0.h). Out of
 * line, as few cells are either. */
static __attribute__((noinline)) uint32_t mode0_reversed(uint16_t f, uint16_t b, bool sel,
                                                         bool inverse, uint32_t highlight,
                                                         uint32_t highlight_text)
{
    const uint16_t hb = (uint16_t)highlight, ht = (uint16_t)highlight_text;
    /* xterm's use_selbg and use_selfg: the color counts, and is neither of
     * the cell's own. */
    const bool use_bg = (highlight & MODE0_HIGHLIGHT) && !mode0_same(hb, f) && !mode0_same(hb, b);
    const bool use_fg =
        (highlight_text & MODE0_HIGHLIGHT) && !mode0_same(ht, f) && !mode0_same(ht, b);
    uint16_t ink, paper;
    if (sel != inverse)
    {
        /* xterm's ReverseOrHilite: the two swapped, then the text color
         * for the ink, and the background color for the paper only where
         * the text color is not used, as its code has it. */
        ink = use_fg ? ht : use_bg ? f : b;
        paper = use_bg ? (use_fg ? b : hb) : f;
    }
    else
    {
        /* A reversed cell in the selection: the two as they are, or the
         * background color for the ink and the text color for the paper. */
        ink = use_bg ? hb : f;
        paper = use_fg ? ht : use_bg ? f : b;
    }
    return (uint32_t)ink | (uint32_t)paper << 16;
}

/* The ink and the paper a cell of attributes attr and colors fg and bg
 * shows, as pixels, by mode0.h's rule, xterm's updatedXtermGC: rev is
 * DECSCNM, sel whether the selection takes the cell, highlight and
 * highlight_text the selection's colors as published, and blink_mask the
 * blink phase. */
static inline __attribute__((always_inline)) void
mode0_cell_pixels(uint16_t attr, uint16_t fg, uint16_t bg, bool rev, bool sel,
                  uint32_t highlight, uint32_t highlight_text, uint8_t blink_mask,
                  uint16_t *ink, uint16_t *paper)
{
    uint16_t f = mode0_color(attr & MODE0_ATTR_BOLD ? mode0_bold(fg) : fg);
    uint16_t b = mode0_color(bg);
    uint16_t t;
    if (rev)
        t = f, f = b, b = t;
    const bool inverse = attr & MODE0_ATTR_REVERSE;
    if (sel || inverse)
    {
        const uint32_t both = mode0_reversed(f, b, sel, inverse, highlight, highlight_text);
        f = (uint16_t)both;
        b = (uint16_t)(both >> 16);
    }
    if (attr & (MODE0_ATTR_CONCEAL | blink_mask))
        f = b;
    *ink = f;
    *paper = b;
}

/* The pixel pairs of every cell of a row: the colors each shows, with the
 * selection's columns [from, to), as the four ways two pixels can land. */
static inline __attribute__((always_inline)) void
mode0_row_pairs(uint32_t *pair, const mode0_cell_t *cell, const mode0_colors_t *colors,
                int cols, uint8_t blink_mask, bool rev, unsigned from, unsigned to,
                uint32_t highlight, uint32_t highlight_text)
{
    for (int i = 0; i < cols; i++, cell++, colors++, pair += 4)
    {
        uint16_t fg, bg;
        mode0_cell_pixels(cell->attributes, colors->fg_color, colors->bg_color, rev,
                          (unsigned)i - from < to - from, highlight, highlight_text, blink_mask,
                          &fg, &bg);
        mode_pair_set(pair, bg, fg);
    }
}

/* One scanline of a terminal row, from the row's pixel pairs. `cols` is the
 * cells drawn and `wide` doubles every pixel across. The callers pass
 * constants, so each combination compiles to its own loop.
 *
 * `lines` says this scanline is a row of the glyph where an underline, a
 * strike or an overline can be. On any other row every cell is its glyph
 * through its pairs, and its attributes are not read. On such a row a cell
 * with no line through it, the common one, is still only that. For any
 * other, `underline` is this row of the underlines, by style, and `line_mask` has
 * the strike and the overline when this row is theirs. A line in the
 * foreground's color is more pixels of the glyph. An underline in a color
 * of its own is drawn over the glyph, a pixel at a time. */
static inline __attribute__((always_inline)) void
mode0_draw(uint16_t *rgb, const mode0_cell_t *cell, const uint32_t *pair, int cols,
           const uint8_t *font_line, uint16_t line_mask, const uint8_t *underline,
           bool lines, bool wide)
{
    if (!lines)
    {
        for (int i = 0; i < cols; i++, cell++, pair += 4, rgb += wide ? 16 : 8)
        {
            const uint8_t bits = font_line[cell->glyph];
            if (wide)
                mode0_render_wide(rgb, bits, pair);
            else
                mode_render_1bpp(rgb, bits, pair);
        }
        return;
    }
    for (int i = 0; i < cols; i++, cell++, pair += 4, rgb += wide ? 16 : 8)
    {
        /* The glyph and the attributes in one load. */
        uint32_t ca;
        memcpy(&ca, cell, sizeof(ca));
        uint8_t bits;
        if (MODE_LIKELY(!(ca & ((uint32_t)MODE0_ATTR_LINES << 16))))
            bits = font_line[(uint16_t)ca];
        else
        {
            const uint16_t attr = (uint16_t)(ca >> 16);
            bits = font_line[(uint16_t)ca];
            if (attr & line_mask)
                bits = 0xFF;
            const uint8_t ul = underline[(attr >> TERM_ATTR_UL_SHIFT) & 7];
            if (ul && (attr & MODE0_ATTR_UL_COLOR))
            {
                /* An underline in its own color, which is with the cell's
                 * other colors. One all the way across is all there is to
                 * draw. Any other is drawn over the glyph, a pixel for each
                 * of its bits. */
                const uint32_t color = mode0_color(mode0_colors[cell - mode0_cells].ul_color) * 0x10001u;
                mode_word_t *w = (mode_word_t *)rgb;
                if (ul == 0xFF)
                {
                    for (int k = 0; k < (wide ? 8 : 4); k++)
                        w[k] = color;
                    continue;
                }
                if (wide)
                {
                    mode0_render_wide(rgb, bits, pair);
                    for (uint32_t u = ul; u; u &= u - 1)
                        w[7 - mode_low_bit(u)] = color;
                }
                else
                {
                    mode_render_1bpp(rgb, bits, pair);
                    for (uint32_t u = ul; u; u &= u - 1)
                        rgb[7 - mode_low_bit(u)] = (uint16_t)color;
                }
                continue;
            }
            bits |= ul;
        }
        if (wide)
            mode0_render_wide(rgb, bits, pair);
        else
            mode_render_1bpp(rgb, bits, pair);
    }
}

/* Whether a cell's color is a default one, the foreground or the
 * background, faint or not (mode0.h). */
static inline __attribute__((always_inline)) bool mode0_is_default(uint16_t color)
{
    return !(color & SCANVIDEO_ALPHA_MASK) &&
           ((color >> MODE0_ENTRY_SHIFT) & MODE0_CODE_DEFAULT);
}

/* The cursor, over its cell as mode0_draw drew it, as xterm's ShowCursor
 * draws it, from the colors the cell shows, by the rule its pairs are
 * built by (mode0_cell_pixels). `cw` is the cell's scanline, `cell` and
 * `colors` the cell's, `cursor` the cursor's word, `rev` DECSCNM, `sel`
 * whether the selection takes the cell, `highlight` and `highlight_text`
 * the selection's colors, `blink_mask` the blink phase, `scanrow` the
 * scanline of the row and `last` the row's last, and `bits` the row of the
 * glyph and its lines a block shows. One cell of a scanline at most, so out
 * of line, once for both fonts. */
static __attribute__((noinline)) void
mode0_draw_cursor(mode_word_t *cw, const mode0_cell_t *cell, const mode0_colors_t *colors,
                  uint32_t cursor, bool rev, bool sel, uint32_t highlight,
                  uint32_t highlight_text, uint8_t blink_mask,
                  bool wide, uint8_t scanrow, uint8_t last, uint8_t bits)
{
    const uint16_t attr = cell->attributes;
    const uint16_t dfg = vterm_specials[VTERM_COLOR_FG - VTERM_PALETTE];
    const uint16_t dbg = vterm_specials[VTERM_COLOR_BG - VTERM_PALETTE];
    /* xterm's ShowCursor, with no EXP_BOGUS_FG: a blank with an ink of its
     * own and no background of its own is not a colored cell under the
     * cursor, so it shows the default ink. A palette ink keeps its faint;
     * an RGB one, whose faint is in its pixel, gives the default ink. */
    uint16_t fgc = colors->fg_color;
    if (mode0_glyph_code(cell->glyph) == ' ' && !mode0_is_default(fgc) &&
        mode0_is_default(colors->bg_color))
        fgc = MODE0_ENTRY(MODE0_DEFAULT_FG,
                          !(fgc & SCANVIDEO_ALPHA_MASK) &&
                              ((fgc >> MODE0_ENTRY_SHIFT) & MODE0_CODE_FAINT));
    uint16_t fg, bg;
    mode0_cell_pixels(attr, fgc, colors->bg_color, rev, sel, highlight, highlight_text, blink_mask,
                      &fg, &bg);
    /* A cell whose ink is its background shows no ink, so it takes as its
     * ink whichever default color is further from that background. */
    if (mode0_same(fg, bg))
        fg = mode0_distance(bg, dbg) < mode0_distance(bg, dfg) ? dfg : dbg;
    const uint16_t color = (uint16_t)mode0_regs.cursor_color;
    /* xterm's outline color: the cursor's, or the ink where that is the
     * background. */
    const uint32_t edge = (mode0_same(color, bg) ? fg : color) * 0x10001u;
    const uint8_t style = MODE0_CURSOR_STYLE(cursor);
    if (style >= 3)
    {
        /* An underline or a bar, over the cell as it shows. Two pixels
         * thick, as xterm's are at these font sizes, but the whole of the
         * cell across or down where xterm's stops a pixel short, and the
         * bar as wide again on a row of double width, as its cells are,
         * where xterm's stays two. */
        if (style >= 5)
        {
            cw[0] = edge;
            if (wide)
                cw[1] = edge;
        }
        else if (scanrow + 2 > last)
            for (int k = 0; k < (wide ? 8 : 4); k++)
                cw[k] = edge;
        return;
    }
    if (cursor & MODE0_CURSOR_UNFOCUSED)
    {
        /* A block with the focus lost: xterm's box, a pixel's outline of
         * the cell over the cell as it shows, from its first pixel to its
         * last, across and down. */
        uint16_t *p = (uint16_t *)cw;
        const int w = wide ? 16 : 8;
        if (scanrow == 0 || scanrow == last)
            for (int k = 0; k < w / 2; k++)
                cw[k] = edge;
        else
            p[0] = p[w - 1] = (uint16_t)edge;
        return;
    }
    /* A block: the cell filled, its glyph and its lines in its background.
     * The cell's default ink is the default foreground, or the default
     * background on a cell shown reversed, by its own reverse, DECSCNM or
     * the selection (xterm's ReverseOrHilite), and its default paper the
     * other. A cursor of the default ink, as the cursor's color starts,
     * fills with the cell's ink, so the block is the cell reversed. A
     * cursor of the default paper fills with the default ink. Any other
     * fills with its own color. Where the fill would be the background,
     * which on xterm hides the glyph in a solid block, it is the ink. */
    rev = (rev != sel) != ((attr & MODE0_ATTR_REVERSE) != 0);
    const uint16_t ink = rev ? dbg : dfg;
    uint16_t fill = color;
    if (mode0_same(color, ink))
        fill = fg;
    else if (mode0_same(color, rev ? dfg : dbg))
        fill = ink;
    if (mode0_same(fill, bg))
        fill = fg;
    MODE_TABLE(cpair, 4);
    mode_pair_set(cpair, fill, bg);
    if (wide)
        mode0_render_wide((uint16_t *)cw, bits, cpair);
    else
        mode_render_1bpp((uint16_t *)cw, bits, cpair);
}

/* A terminal scanline: `ch` is 8 on the 320-wide canvases, where the
 * 40-column terminal is drawn, and 16 on the others. */
static inline __attribute__((always_inline)) bool
mode0_render_term(int16_t scanline_id, int16_t begin, uint16_t *rgb, const int ch,
                  const int width, const uint8_t *font, vga_fill_cache_t *cache)
{
    /* A row past the terminal's would read past its rows, so a row copied
     * part way through a reprogram, with another mode's config, draws
     * nothing. */
    scanline_id -= begin;
    if ((uint16_t)scanline_id >= TERM_MAX_HEIGHT * ch)
        return false;
    uint8_t logical_row = (uint8_t)(scanline_id / ch);
    uint8_t scanrow = (uint8_t)(scanline_id % ch);
    uint32_t row_word = mode0_regs.row[logical_row];
    /* A smooth scroll moving: a scanline of its rows shows the rows where
     * they were, moved on by the scanlines it has gone, with the row going
     * out still at the edge it leaves by. A row of the ghost has no cursor. */
    const uint32_t region = mode0_regs.smooth;
    if ((region & MODE0_SMOOTH_ON) && logical_row >= MODE0_SMOOTH_TOP(region) &&
        logical_row <= MODE0_SMOOTH_BOT(region))
    {
        const unsigned moved = mode0_smooth_moved(mode0_regs.smooth_frame,
                                                  (uint32_t)vga_frame_count(), (unsigned)ch);
        if (moved < (unsigned)ch)
        {
            const uint8_t top = MODE0_SMOOTH_TOP(region);
            const uint8_t bot = MODE0_SMOOTH_BOT(region);
            const bool up = region & MODE0_SMOOTH_UP;
            const unsigned at = (unsigned)(scanline_id - top * ch) +
                                (up ? moved : (unsigned)ch - moved);
            const unsigned r = at / ch;
            scanrow = (uint8_t)(at % ch);
            if (up ? r == 0 : r == (unsigned)(bot - top + 1))
            {
                row_word = mode0_regs.smooth_ghost;
                logical_row = 0xFF;
            }
            else
            {
                logical_row = (uint8_t)(top + r - (up ? 1 : 0));
                row_word = mode0_regs.row[logical_row];
            }
        }
    }
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
    /* A row of both of the font's tables. */
    const uint8_t *font_line = &font[glyph_row * 256];
    const uint8_t blink_mask = MODE0_SCREEN_BLINK(screen);
    const uint16_t line_mask =
        (uint16_t)((glyph_row == ch / 2 ? TERM_ATTR_STRIKE : 0) |
                   (glyph_row == 0 ? TERM_ATTR_OVERLINE : 0));
    /* The underlines are on the glyph's last three rows. */
    const uint8_t underline_row = (uint8_t)(glyph_row + 3 > last ? glyph_row + 3 - last : 0);
    const uint8_t *underline = (ch == 16 ? mode0_underline16 : mode0_underline8)[underline_row];
    const bool lines = line_mask || underline_row;
    const mode0_cell_t *cell = &mode0_cells[MODE0_ROW_CELL(row_word)];
    const mode0_colors_t *colors = &mode0_colors[MODE0_ROW_CELL(row_word)];
    const bool wide = size != TERM_LINE_NORMAL;
    const int cols = wide ? width / 2 : width;
    /* The selection's columns of the row, which its word carries. */
    const unsigned sel_from = MODE0_ROW_FROM(row_word), sel_to = MODE0_ROW_TO(row_word);
    /* The row's pairs are built for the first of its scanlines a thread
     * draws in a frame, and again when the row, the selection's columns of
     * it, the screen's look, which holds the blink phase and DECSCNM, or
     * the selection's colors change part way down it. */
    /* The rest of the key is zero, as it is for a row built here. */
    const uint32_t highlight = mode0_regs.highlight;
    const uint32_t highlight_text = mode0_regs.highlight_text;
    const uint32_t key[9] = {VGA_FILL_MODE0_ROW, row_word, screen, (uint32_t)vga_frame_count(),
                             highlight, highlight_text};
    if (!vga_fill_cache_hit(cache, key))
    {
        mode0_row_pairs(cache->row_pair[0], cell, colors, cols, blink_mask, reverse, sel_from,
                        sel_to, highlight, highlight_text);
        memcpy(cache->key, key, sizeof(key));
    }
    const uint32_t *pair = cache->row_pair[0];
    if (wide && lines)
        mode0_draw(rgb, cell, pair, width / 2, font_line, line_mask, underline, true, true);
    else if (wide)
        mode0_draw(rgb, cell, pair, width / 2, font_line, line_mask, underline, false, true);
    else if (lines)
        mode0_draw(rgb, cell, pair, width, font_line, line_mask, underline, true, false);
    else
        mode0_draw(rgb, cell, pair, width, font_line, line_mask, underline, false, false);
    /* The cursor, over the cell drawn as it shows. DECSCUSR's 2, 4 and 6
     * are steady, so they draw whether or not the blink has it lit, and so
     * does every style with the focus lost, as xterm's; draw.c times the
     * blink and leaves the style to the renderer. */
    const uint8_t cursor_style = MODE0_CURSOR_STYLE(cursor);
    if (logical_row == MODE0_CURSOR_Y(cursor) &&
        (cursor & MODE0_CURSOR_ENABLED) &&
        ((cursor & (MODE0_CURSOR_LIT | MODE0_CURSOR_UNFOCUSED)) ||
         cursor_style == 2 ||
         cursor_style == 4 ||
         cursor_style == 6))
    {
        uint8_t cx = MODE0_CURSOR_X(cursor);
        /* A column past the row's, from a row copied part way through a
         * change, is the last. */
        if (cx >= cols)
            cx = (uint8_t)(cols - 1);
        /* The row of the glyph a block shows, with its lines, but none of
         * a cell concealed or blinking off. */
        const uint16_t cattr = cell[cx].attributes;
        uint8_t bits = 0;
        if (!(cattr & (MODE0_ATTR_CONCEAL | blink_mask)))
        {
            bits = font_line[cell[cx].glyph];
            if (cattr & line_mask)
                bits = 0xFF;
            bits |= underline[(cattr >> TERM_ATTR_UL_SHIFT) & 7];
        }
        mode0_draw_cursor((mode_word_t *)(rgb + (uint32_t)cx * (wide ? 16 : 8)), &cell[cx],
                          &colors[cx], cursor, reverse,
                          (unsigned)cx - sel_from < sel_to - sel_from, highlight, highlight_text,
                          blink_mask, wide, scanrow, last, bits);
    }
    return true;
}

/* The config of a terminal row is the scanline the terminal begins at, so
 * each row says where it is in the terminal. A render on the other core
 * copies the row whole, and a begin kept apart from the rows could pair an
 * old row with a new begin, or a new row with an old one. */
static bool
mode0_render(uint16_t options, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr,
             vga_fill_cache_t *cache)
{
    (void)options;
    if (width == 320)
        return mode0_render_term(scanline_id, (int16_t)config_ptr, rgb, 8, 40, font8, cache);
    else
        return mode0_render_term(scanline_id, (int16_t)config_ptr, rgb, 16, 80, font16, cache);
}
#pragma GCC pop_options

vga_fill_fn_t mode0_fill_fn(uint16_t options)
{
    return options ? NULL : mode0_render;
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
                           (uint16_t)scanline_begin, 0, 0))
    {
        vterm_place(cols, rows, scanline_begin, (uint8_t)vga_term_row_px(vga_canvas_width()));
        return true;
    }
    return false;
}
