/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_CELL_MODE0_H_
#define _CORE_TERM_CELL_MODE0_H_

/* Mode 0's kind of cell (cell.h), which includes this: the cell as mode 0
 * draws it (core/vga/mode/mode0.h), with its colors.
 *
 * A cell's glyph and attributes are one word. The glyph is where the font
 * has it, so a scanline draws any cell with one lookup. The attributes
 * hold the lines through a cell (underline, strike, overline), which a
 * scanline reads only on the rows of the glyph where a line can be, and
 * there it branches once per cell on the word being a bare glyph.
 *
 * A cell's colors are in a table beside the cells, mode0_colors, at the
 * same index, so they go wherever the cell goes. A cell names its colors: a
 * palette entry or a default color as an entry of mode 0's palette, an RGB
 * color as the pixel. The pen chooses which as each cell is written, and a
 * palette or default color that changes afterwards is one write to the
 * palette, which every cell drawn with it shows on the next frame. */

#include "core/term/term.h"
#include "core/vga/mode/mode0.h"
#include "core/vga/pixel_format.h"
#include <stdbool.h>
#include <stdint.h>

typedef mode0_cell_t cell_t;

// A fill: the cell and its colors. An erase is a fill with a blank cell,
// DECALN's is a fill with an E, and REP's is a fill with its glyph as the
// pen draws it.
typedef struct
{
    mode0_cell_t cell;
    mode0_colors_t colors;
    bool selective;
} cell_fill_t;

// The colors of a blank (cell_blank).
typedef struct
{
    uint16_t fg;
    uint16_t bg;
} cell_side_t;

typedef struct
{
    mode0_cell_t cell[CELL_STRIP_MAX];
    mode0_colors_t colors[CELL_STRIP_MAX];
} cell_strip_t;

/* ---- colors ------------------------------------------------------------ */

// A cell's color at half brightness: an RGB pixel halved, or the faint twin
// of an entry.
static inline uint16_t cell_faint(uint16_t color)
{
    if (color & SCANVIDEO_ALPHA_MASK)
        return mode0_faint(color);
    return (uint16_t)(color | (MODE0_CODE_FAINT << MODE0_ENTRY_SHIFT));
}

// A pen color before bold, faint and iCE colors, as a cell holds it: the
// palette entry, the RGB pixel, or the default (dflt, MODE0_DEFAULT_FG or
// _BG).
static inline uint16_t cell_base(const term_color_t *c, unsigned dflt)
{
    switch (c->kind)
    {
    case TERM_COLOR_INDEX:
        return MODE0_ENTRY(c->index, false);
    case TERM_COLOR_RGB:
        return c->rgb | SCANVIDEO_ALPHA_MASK;
    default:
        return MODE0_ENTRY(dflt, false);
    }
}

// A pen's ink as a cell holds it, written or erased: faint halves each
// channel, and bold is the cell's to draw (mode0_cell_shows).
static inline uint16_t cell_pen_ink(const term_pen_t *pen)
{
    const uint16_t fg = cell_base(&pen->fg, MODE0_DEFAULT_FG);
    return pen->faint ? cell_faint(fg) : fg;
}

// A pen's background. In iCE colors mode (?33) SGR 5/6 brighten a
// background of 0-7 or the default, the IBM VGA hack the BBS scene relies
// on; a brightened color is the entry it was brightened to.
static inline uint16_t cell_pen_bg(const term_t *term, const term_pen_t *pen)
{
    bool bright = term->ice_colors && (pen->sgr_attr & TERM_ATTR_ANY_BLINK);
    if (bright && pen->bg.kind == TERM_COLOR_INDEX && pen->bg.index < 8)
        return MODE0_ENTRY(pen->bg.index + 8, false);
    if (bright && pen->bg.kind == TERM_COLOR_DEFAULT)
        return MODE0_ENTRY(8, false);
    return cell_base(&pen->bg, MODE0_DEFAULT_BG);
}

/* ---- fills and sides --------------------------------------------------- */

// An erase takes the pen's colors, not reversed, and none of its
// attributes, as xterm and Linux do.
static inline cell_side_t cell_side_pen(const term_t *term)
{
    const term_pen_t *pen = term_pen(term);
    const cell_side_t side = {cell_pen_ink(pen), cell_pen_bg(term, pen)};
    return side;
}

static inline cell_side_t cell_side_default(void)
{
    const cell_side_t side = {MODE0_ENTRY(MODE0_DEFAULT_FG, false),
                              MODE0_ENTRY(MODE0_DEFAULT_BG, false)};
    return side;
}

// A blank cell in these colors, with no attributes and no guard, written
// or not. The foreground matters too, because the cursor is drawn from the
// cell it sits on.
static inline cell_fill_t cell_blank(uint8_t code, cell_side_t side, bool selective,
                                     bool written)
{
    const cell_fill_t e = {{code, written ? MODE0_ATTR_WRITTEN : 0},
                           {side.fg, side.bg, side.fg},
                           selective};
    return e;
}

// The attributes hold whether the cells are written.
static inline bool cell_fill_same(const cell_fill_t *a, const cell_fill_t *b)
{
    return a->selective == b->selective && a->cell.glyph == b->cell.glyph &&
           a->cell.attributes == b->cell.attributes &&
           a->colors.fg_color == b->colors.fg_color &&
           a->colors.bg_color == b->colors.bg_color &&
           a->colors.ul_color == b->colors.ul_color;
}

// The cell a character lands as: the pen's colors and attributes, and the
// glyph where the font has it (mode0.h), in the font's rows for the
// screen's width: 8 on the 40-column screen, drawn on the 320-wide
// canvases, and 16 on the 80. In iCE colors mode (?33) a blinking pen's
// background is the bright one (cell_pen_bg), and the cell keeps its
// blink, as SyncTERM's does: ?35 stops every cell blinking. An underline
// SGR 58 colored is flagged, so the renderer reads its color; any other
// holds the foreground's color. A cell written while DECSCA or SPA protects
// is guarded, and every one is written (cell.h). Bold, SGR 7 and SGR 8 are
// bits of the cell, which the renderer draws (mode0_cell_shows), so DECCARA
// and DECRARA change bits alone.
static inline cell_fill_t cell_from_pen(const term_t *term, uint8_t width, uint8_t code,
                                        unsigned set)
{
    const term_pen_t *pen = term_pen(term);
    const unsigned rows = width == 40 ? 8 : 16;
    cell_fill_t e;
    const uint16_t f = cell_pen_ink(pen);
    e.colors.fg_color = f;
    e.colors.bg_color = cell_pen_bg(term, pen);
    uint16_t attributes = (uint16_t)((pen->sgr_attr & TERM_ATTR_CELL_MASK) | MODE0_ATTR_WRITTEN);
    if (pen->ul_set && (attributes & TERM_ATTR_UL_MASK))
        attributes |= MODE0_ATTR_UL_COLOR;
    if (term_protecting(term))
        attributes |= MODE0_ATTR_GUARD;
    if (pen->bold)
        attributes |= MODE0_ATTR_BOLD;
    if (pen->reverse)
        attributes |= MODE0_ATTR_REVERSE;
    if (pen->conceal)
        attributes |= MODE0_ATTR_CONCEAL;
    uint16_t glyph = code;
    if (set == CELL_SET_DEC)
        glyph = MODE0_GLYPH_DEC(rows, code);
    else if (set == CELL_SET_TECH)
        glyph = MODE0_GLYPH_TECH(rows, code);
    else if (set == CELL_SET_ITALIC)
        glyph = MODE0_GLYPH_ITALIC(rows, code);
    e.cell.glyph = glyph;
    e.cell.attributes = attributes;
    e.colors.ul_color = pen->ul_set ? cell_base(&pen->ul, MODE0_DEFAULT_FG) : f;
    e.selective = false;
    return e;
}

/* ---- cells ------------------------------------------------------------- */

// Where a cell is in mode 0's cells, and so its colors in mode0_colors.
static inline uint32_t cell_at(const mode0_cell_t *cell)
{
    return (uint32_t)(cell - mode0_cells);
}

static inline mode0_colors_t *cell_colors(const mode0_cell_t *cell)
{
    return &mode0_colors[cell_at(cell)];
}

static inline bool cell_guarded(const mode0_cell_t *cell)
{
    return cell->attributes & MODE0_ATTR_GUARD;
}

// A character: its word and its three colors.
static inline void cell_put1(mode0_cell_t *at, const cell_fill_t *e)
{
    *at = e->cell;
    *cell_colors(at) = e->colors;
}

// Runs of cells, out of line (cell_mode0.c). Everything that writes or
// moves more than a cell does it there.
void cell_run_fill(mode0_cell_t *dst, unsigned n, const cell_fill_t *e);
void cell_run_fill_sel(mode0_cell_t *row, unsigned from, unsigned to, const cell_fill_t *e);
void cell_run_move(mode0_cell_t *dst, const mode0_cell_t *src, unsigned n);
void cell_wipe(mode0_cell_t *row, unsigned from, unsigned to, bool selective);
void cell_marks(mode0_cell_t *row, unsigned from, unsigned to, unsigned set, unsigned clear,
                unsigned toggle);
void cell_strip_keep(cell_strip_t *strip, const mode0_cell_t *row, unsigned from,
                     unsigned end, unsigned width);
void cell_strip_put(const cell_strip_t *strip, mode0_cell_t *row, unsigned from,
                    unsigned end, unsigned width);
void cell_strip_move(mode0_cell_t *to, const mode0_cell_t *row, unsigned from, unsigned end,
                     unsigned width);

// A cell shows something: a character other than a space, a line through
// it, a background other than the default one, or its colors swapped. The
// background is the one drawn, iCE colors' bright one included. A swapped
// cell counts whatever its colors are.
static inline bool cell_shows(const mode0_cell_t *cell)
{
    return mode0_glyph_code(cell->glyph) != ' ' ||
           (cell->attributes & (MODE0_ATTR_LINES | MODE0_ATTR_REVERSE)) ||
           cell_colors(cell)->bg_color != MODE0_ENTRY(MODE0_DEFAULT_BG, false);
}

// The character the glyph is and the set it is in, the italics' the code
// page's, whether it was written and its guard.
_Static_assert(MODE0_ATTR_WRITTEN == CELL_TEXT_WRITTEN,
               "the written bit is where the text has it");
static inline uint16_t cell_text(const mode0_cell_t *cell)
{
    const uint16_t glyph = cell->glyph;
    const unsigned set = mode0_glyph_is_dec(glyph)    ? CELL_SET_DEC
                         : mode0_glyph_is_tech(glyph) ? CELL_SET_TECH
                                                      : CELL_SET_PAGE;
    return (uint16_t)(mode0_glyph_code(glyph) | set << CELL_TEXT_SET_SHIFT |
                      (cell->attributes & MODE0_ATTR_WRITTEN) |
                      (cell_guarded(cell) ? CELL_TEXT_GUARD : 0));
}

#endif /* _CORE_TERM_CELL_MODE0_H_ */
