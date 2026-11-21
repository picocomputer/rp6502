/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_GRID_H_
#define _CORE_TERM_GRID_H_

/* The terminal at one size: a grid's cursors, margins, tab stops and the
 * rows of its screens, and what the terminal's parser hands each grid. The parser decodes
 * a byte and does everything that is the same at every size; what moves the
 * cursor or the rows, it hands to every grid with one of the grid_op_t.
 * The sequence's numbers are in the terminal's csi_param. */

#include "core/sys/sst.h"
#include "core/term/screen.h"
#include "core/term/term.h"
#include <stdbool.h>
#include <stdint.h>

// The terminal at one size. Each screen, primary and alternate, has its own
// cursor position, saved position and ring of rows, which carry their line
// attributes; the margins
// and the tab stops belong to the grid. shown points at the screen showing:
// term_grid_init and term_grid_copy start it, and term_show sets it. The
// cursor's column never reaches the width: writing the last column sets
// wrap_pending instead, as on a VT102. The left and right margins are the
// whole width until DECSLRM sets them. Where the screens keep cells,
// cancels goes up at each scroll, index, clear and change of height of the
// grid, and at nothing else, each of which ends the grid's smooth scroll
// (console_term.h).
struct term_grid
{
    uint8_t width;
    uint8_t height;
    uint8_t margin_top, margin_bot;
    uint8_t margin_left, margin_right;
    screen_t screen[2]; // the primary, then the alternate
    screen_t *shown;    // the screen showing
    uint8_t tab_stops[TERM_TAB_BITMAP_BYTES]; // 1 bit per column
#if TERM_CELLS
    uint32_t cancels;
#endif
};

/* The cursor of the screen showing.
 *
 * This stays static inline and reads only the shown screen's pos, never the
 * ring, the line attributes or anything in another file: term_pos, and
 * grid.c's grid_in_columns, which reads only the margins, run in the RIA's
 * HOST_IN_RAM_FUNC drain path (grid_*_scrolls, term_holds,
 * term_byte_scrolls and console_smooth_grid), where a call to a function in
 * flash cannot go. term_pos carries the annotation too, for when it is not
 * inlined. */
static inline HOST_IN_RAM_FUNC term_pos_t *term_pos(const term_grid_t *g)
{
    return &g->shown->pos;
}

// The most rows the grid has: the slots of each screen's ring.
static inline uint8_t grid_rows(const term_grid_t *g)
{
    return g->screen[0].rows;
}

// The columns of row y: half the width on a double-size line.
static inline unsigned term_cols(const term_grid_t *g, uint8_t y)
{
    return screen_size(g->shown, y) ? g->width / 2u : g->width;
}

// The last column a glyph or a move to the right reaches from the cursor:
// the right margin for a cursor at or left of it, otherwise the last column
// of its row.
static inline unsigned term_right_stop(const term_grid_t *g)
{
    const term_pos_t *pos = term_pos(g);
    unsigned last = term_cols(g, pos->y) - 1;
    return pos->x <= g->margin_right && g->margin_right < last ? g->margin_right : last;
}

// The first column a CR or a move to the left reaches: the left margin for
// a cursor at or right of it, otherwise the first column.
static inline unsigned term_left_stop(const term_grid_t *g)
{
    return term_pos(g)->x >= g->margin_left ? g->margin_left : 0;
}

// Whether col is a tab stop.
static inline bool grid_tab_is_set(const term_grid_t *g, uint8_t col)
{
    return (g->tab_stops[col >> 3] & (1u << (col & 7))) != 0;
}

// The pixel height of one character cell: 8x8 on the 320-wide canvases, where
// the 40-column terminal is drawn, 8x16 on the others.
static inline unsigned term_cell_height(const term_grid_t *g)
{
    return g->width == 40 ? 8 : 16;
}

// What the parser hands each grid.
typedef enum
{
    GRID_PRINT,   // ch, a glyph
    GRID_C0,      // ch, BS, HT, LF, VT, FF or CR
    GRID_CSI,     // ch, the final byte of a CSI with no private marker or intermediate
    GRID_REP,     // REP, the last glyph again
    GRID_SELECTIVE, // ch, J or K: DECSED or DECSEL
    GRID_SHIFT,   // ch, the final byte of SL (CSI SP @) or SR (CSI SP A)
    GRID_COLUMNS, // ch, the final byte of DECIC (CSI ' }) or DECDC (CSI ' ~)
    GRID_RECT,    // ch, the final byte of a rectangle sequence, CSI ... $ x z { v r t
    GRID_LRMM,    // ch, DECLRMM set or reset
    GRID_ESC,     // ch, the final byte of ESC D, E, H, M, 6 or 9
    GRID_LINE,    // ch, the final byte of ESC # 3 to 6 and 8
    GRID_VT52,    // ch, the letter of a VT52 cursor or erase sequence
    GRID_VT52_Y,  // ch, the column of VT52's ESC Y; the row is csi_param[0]
    GRID_SAVE,    // DECSC: the position
    GRID_RESTORE, // DECRC: the position
    GRID_HOME,    // the top left, or the region's under DECOM
    GRID_STAY,    // the cursor where it is, its row touched and its wrap ended
    GRID_ALT_IN,  // the alternate screen's cursor takes the primary's
    GRID_ALT_OUT, // the primary screen's cursor takes the alternate's
    GRID_CLEAR,   // the screen showing erased, every line single size
    GRID_RESET,   // RIS: the margins, the tabs and the saved positions
    GRID_DECSTR,  // DECSTR: the margins and the saved positions
    GRID_DECCOLM, // DECCOLM with ?40: the screen erased, single size, full margins, home
} grid_op_t;

void grid_do(term_t *t, term_grid_t *g, grid_op_t op, uint8_t ch);

// Whether an index (LF, VT, FF, IND, NEL), a reverse index (RI), or a glyph
// written now would scroll g's region: the cursor on the margin it scrolls
// from, between the left and right margins, and for a glyph, a wrap
// pending with DECAWM on.
bool grid_index_scrolls(const term_grid_t *g);
bool grid_ri_scrolls(const term_grid_t *g);
bool grid_glyph_scrolls(const term_t *t, const term_grid_t *g);

// DECCIR's restore: the showing screen's cursor has a wrap pending, or not.
static inline void grid_restore_wrap(term_grid_t *g, bool on)
{
    term_pos(g)->wrap_pending = on;
}

// DECTABSR's restore: the tab stops are the columns set in cols, a bit a
// column from bit 0 of cols[0], as the parser gathers them; those past the
// width are left out.
void grid_set_tabs(term_grid_t *g, const uint16_t *cols);

/* g becomes a copy of from, showing the same screen, with its own line
 * attributes in lines, rows a screen, and no cells. Its rings start in
 * order, each row with the line attributes of the same row of from's. */
void term_grid_copy(term_grid_t *g, const term_grid_t *from, uint8_t *lines, uint8_t rows);

// A grid of width columns by 30 rows (fewer if it holds fewer), with rows
// rows a screen: its rings in row_idx, rows bytes for each of the two
// screens, and its line attributes in lines, a byte for each row of memory
// of each screen (screen_init). row_idx may be NULL, for screens that will
// have no cells, and then lines holds rows bytes for each. The screens have
// no cells until their owner gives them theirs (screen_set_cells).
void term_grid_init(term_grid_t *g, uint8_t width, uint8_t *lines, uint8_t *row_idx,
                    uint8_t rows);

// A grid's rows, one at a time, as the view grows or shrinks. A grow or
// shrink with the cursor on the bottom row moves the screen, not the cursor.
void term_set_height(term_t *t, term_grid_t *g, uint8_t height);

// A grid's columns, as a program sets them: the cursors stay inside the
// rows, a pending wrap ends, the tab stops past the old width are the
// default ones, and the margins reset.
void term_set_width(term_t *t, term_grid_t *g, uint8_t width);

/* ---- savestate ----------------------------------------------------------
 *
 * The rows of a grid for its owner's SST_TABLE (sst.h), field by field, so
 * no padding reaches the blob. They leave out its rings and their line
 * attributes, which are the owner's arrays. The screens' rows, which are
 * the same for both, are saved once, the primary's. */

#define TERM_POS_SST_ROWS(Y, p)                                        \
    Y(U8, p.x) Y(U8, p.y) Y(BOOL, p.wrap_pending)

#define TERM_GRID_SST_ROWS(Y, g)                                       \
    Y(U8, g.width) Y(U8, g.height) Y(U8, g.screen[0].rows)                     \
    Y(U8, g.margin_top) Y(U8, g.margin_bot)                            \
    Y(U8, g.margin_left) Y(U8, g.margin_right)                         \
    TERM_POS_SST_ROWS(Y, g.screen[0].pos)                              \
    TERM_POS_SST_ROWS(Y, g.screen[0].saved)                            \
    TERM_POS_SST_ROWS(Y, g.screen[1].pos)                              \
    TERM_POS_SST_ROWS(Y, g.screen[1].saved)                            \
    Y(BYTES, g.tab_stops)

/* Whether a grid TERM_GRID_SST_ROWS loaded fits storage for rows rows, and
 * everything that indexes the cells is in range. False, after sst_refuse,
 * when not. */
bool term_grid_sst_ok(sst_cursor_t *c, const term_grid_t *g, uint8_t rows);

/* Whether the line attributes of a grid's screens are ones there are, and
 * each screen's cursor is inside its line's columns. For after the rings
 * and the line attributes are loaded. False, after sst_refuse, when not. */
bool term_grid_sst_lines_ok(sst_cursor_t *c, const term_grid_t *g);

/* A grid's rows and then, for each screen, its line attributes, its ring's
 * turn and the rows above row 0, for an owner whose grid is not always
 * followed and so needs an FN row of its own, and whose screens have no
 * ring and no cells. A grid of other rows than g has is refused. */
#define TERM_GRID_SST_SIZE(rows) (19 + TERM_TAB_BITMAP_BYTES + 2 * ((rows) + 2))
void term_grid_sst_put(sst_cursor_t *c, const term_grid_t *g);
bool term_grid_sst_get(sst_cursor_t *c, term_grid_t *g);

#endif /* _CORE_TERM_GRID_H_ */
