/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/cells.h"
#include "core/term/grid.h"
#include "core/term/term.h"
#include <assert.h>
#include <string.h>

// The terminal at one size: the cursor's moves, the margins, the tab stops,
// scrolling, erasing, the rectangles and the line sizes. Everything here
// runs once for each grid the terminal is followed at, after term.c has
// decoded the sequence.
//
// The left and right margins (DECSLRM) are xterm's. A cursor between them
// stays between them: a glyph wraps at the right margin, and the moves that
// stop at an edge of the screen stop at a margin. A cursor a program put
// outside them is stopped by the screen's edges until a wrap or a carriage
// return brings it back. Scrolling and editing move only the columns between
// them, and only for a cursor that is between them.

static inline uint16_t grid_param(const term_t *t, uint8_t i, uint16_t dflt)
{
    uint16_t v = (i < t->csi_param_count) ? t->csi_param[i] : 0;
    return v ? v : dflt;
}

/* ---- line sizes -------------------------------------------------------- */

// Rows [from, to) of the screen showing single size again, as an erase
// leaves them.
static void grid_lines_normal(term_grid_t *g, unsigned from, unsigned to)
{
    uint8_t *ls = term_lines(g);
    for (unsigned i = from; i < to; i++)
        ls[i] = TERM_LINE_NORMAL;
}

// The line sizes of rows [top, bot] move n rows up or down with the rows,
// single-size rows coming in.
static void grid_lines_scroll(term_grid_t *g, uint8_t top, uint8_t bot, uint8_t n,
                              bool up)
{
    uint8_t *ls = term_lines(g);
    if (up)
    {
        for (unsigned i = top; i + n <= bot; i++)
            ls[i] = ls[i + n];
        grid_lines_normal(g, (unsigned)(bot - n + 1), (unsigned)bot + 1);
    }
    else
    {
        for (int i = (int)bot; i - (int)n >= (int)top; i--)
            ls[i] = ls[i - n];
        grid_lines_normal(g, top, (unsigned)top + n);
    }
}

/* ---- the cursor -------------------------------------------------------- */

// Move the cursor, 0-indexed, clamped to the screen and to the columns of its
// row. Every move ends a pending wrap, and the destination row is touched so a
// glyph can land on it.
static void grid_goto(term_t *t, term_grid_t *g, unsigned x, unsigned y)
{
    term_pos_t *pos = term_pos(g);
    if (y >= g->height)
        y = g->height - 1u;
    unsigned cols = term_cols(g, (uint8_t)y);
    if (x >= cols)
        x = cols - 1u;
    pos->x = (uint8_t)x;
    pos->y = (uint8_t)y;
    pos->wrap_pending = false;
    cells_touch(t, g, (uint8_t)y);
}

static void grid_reset_tabs(term_grid_t *g)
{
    memset(g->tab_stops, 0, sizeof(g->tab_stops));
    for (unsigned col = 8; col < g->width; col += 8)
        g->tab_stops[col >> 3] |= (uint8_t)(1u << (col & 7));
}

static void grid_full_columns(term_grid_t *g)
{
    g->margin_left = 0;
    g->margin_right = (uint8_t)(g->width - 1);
}

static void grid_full_margins(term_grid_t *g)
{
    g->margin_top = 0;
    g->margin_bot = (uint8_t)(g->height - 1);
    grid_full_columns(g);
}

// Whether the left and right margins are the whole width.
static inline bool grid_all_columns(const term_grid_t *g)
{
    return g->margin_left == 0 && g->margin_right == g->width - 1;
}

// Whether column x is between the left and right margins.
static inline bool grid_in_columns(const term_grid_t *g, unsigned x)
{
    return x >= g->margin_left && x <= g->margin_right;
}

// The last column a glyph or a move to the right reaches from the cursor:
// the right margin for a cursor at or left of it, otherwise the last column
// of its row.
static unsigned grid_right_stop(const term_grid_t *g)
{
    const term_pos_t *pos = term_pos(g);
    unsigned last = term_cols(g, pos->y) - 1;
    return pos->x <= g->margin_right && g->margin_right < last ? g->margin_right : last;
}

// The first column a move to the left reaches: the left margin for a cursor
// at or right of it, otherwise the first column.
static unsigned grid_left_stop(const term_grid_t *g)
{
    return term_pos(g)->x >= g->margin_left ? g->margin_left : 0;
}

void term_grid_init(term_grid_t *g, uint8_t width, uint8_t *lines, uint8_t rows)
{
    memset(g, 0, sizeof *g);
    g->width = width;
    g->rows = rows;
    g->height = rows < TERM_STD_HEIGHT ? rows : TERM_STD_HEIGHT;
    g->lines = lines;
    g->cur = &g->pos[0];
    g->ls = lines;
    memset(lines, TERM_LINE_NORMAL, 2u * rows);
    grid_full_margins(g);
    grid_reset_tabs(g);
}

void term_grid_copy(term_grid_t *g, const term_grid_t *from, uint8_t *lines, uint8_t rows)
{
    *g = *from;
    g->rows = rows;
    g->lines = lines;
    memset(lines, TERM_LINE_NORMAL, 2u * rows);
    unsigned n = from->rows < rows ? from->rows : rows;
    memcpy(lines, from->lines, n);
    memcpy(lines + rows, from->lines + from->rows, n);
    if (g->height > rows)
        g->height = rows;
}

void term_set_width(term_t *t, term_grid_t *g, uint8_t width)
{
    (void)t;
    assert(width >= 1);
    for (unsigned col = (g->width + 7u) & ~7u; col < width; col += 8)
        if (col)
            g->tab_stops[col >> 3] |= (uint8_t)(1u << (col & 7));
    g->width = width;
    for (int s = 0; s < 2; s++)
    {
        term_pos_t *ps[2] = {&g->pos[s], &g->saved[s]};
        for (int i = 0; i < 2; i++)
        {
            term_pos_t *p = ps[i];
            unsigned cols = g->lines[s * g->rows + p->y] ? width / 2u : width;
            if (p->x >= cols)
                p->x = (uint8_t)(cols ? cols - 1 : 0);
            p->wrap_pending = false;
        }
    }
    grid_full_margins(g);
}

/* ---- height ------------------------------------------------------------ */

void term_set_height(term_t *t, term_grid_t *g, uint8_t height)
{
    assert(height >= 1 && height <= g->rows);
    term_pos_t *pos = term_pos(g);
    uint8_t *ls = term_lines(g);
    while (height != g->height)
    {
        if (height > g->height)
        {
            g->height++;
            if (pos->y == g->height - 2)
            {
                // The cursor is on the bottom row, so the view grows at the
                // top: everything moves down a row and the row above comes
                // into view.
                pos->y++;
                for (int i = g->height - 1; i > 0; i--)
                    ls[i] = ls[i - 1];
                ls[0] = TERM_LINE_NORMAL;
                cells_height(g, true, true);
            }
            else
            {
                // A fresh row at the bottom.
                ls[g->height - 1] = TERM_LINE_NORMAL;
                cells_height(g, true, false);
            }
        }
        else
        {
            g->height--;
            if (pos->y == g->height)
            {
                // The cursor is on the bottom row, so the top row goes out of
                // view and joins the scrollback.
                pos->y--;
                for (unsigned i = 0; i < g->height; i++)
                    ls[i] = ls[i + 1];
                cells_height(g, false, true);
            }
            else
                // The bottom row is no longer visible.
                cells_height(g, false, false);
        }
    }
    grid_full_margins(g);

    // The resize loop only tracks the showing screen's cursor. Any other
    // cursor row (the other screen's and both saved positions) can exceed
    // the new height after a shrink; clamp them so a later swap or DECRC
    // doesn't strand the cursor below the viewport.
    uint8_t max_y = (uint8_t)(g->height - 1);
    term_pos_t *other = &g->pos[t->alt_active ? 0 : 1];
    if (other->y > max_y)
        other->y = max_y;
    for (int s = 0; s < 2; s++)
        if (g->saved[s].y > max_y)
            g->saved[s].y = max_y;
}

/* ---- scrolling --------------------------------------------------------- */

// Erase columns [from, to) of a row that may still be owed something.
static void grid_erase_cols(term_t *t, term_grid_t *g, uint8_t y, unsigned from,
                            unsigned to, bool selective)
{
    cells_touch(t, g, y);
    cells_erase(t, g, y, from, to, selective);
}

// Copy the h rows of w columns at (sy, sx) to (dy, dx). The two may overlap:
// the rows go in the order that reads each before it is written. A
// double-size line gives and takes only the columns it has.
static void grid_copy(term_t *t, term_grid_t *g, unsigned sy, unsigned sx,
                      unsigned dy, unsigned dx, unsigned h, unsigned w)
{
    for (unsigned i = 0; i < h; i++)
    {
        unsigned k = dy > sy ? h - 1 - i : i;
        unsigned from = term_cols(g, (uint8_t)(sy + k)), to = term_cols(g, (uint8_t)(dy + k));
        if (sx >= from || dx >= to)
            continue;
        unsigned n = w;
        if (n > from - sx)
            n = from - sx;
        if (n > to - dx)
            n = to - dx;
        cells_copy(t, g, (uint8_t)(sy + k), sx, (uint8_t)(dy + k), dx, n);
    }
}

// Scroll the inclusive region [top, bot] up or down by count rows, erased rows
// coming in. The caller places the cursor and touches its row. Between left
// and right margins only the cells between them move.
static void grid_scroll(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot,
                        uint16_t count, bool up)
{
    if (top > bot || count == 0)
        return;
    uint8_t region_h = (uint8_t)(bot - top + 1);
    uint8_t n = count > region_h ? region_h : (uint8_t)count;
    if (grid_all_columns(g))
    {
        grid_lines_scroll(g, top, bot, n, up);
        cells_scroll(t, g, top, bot, n, up);
        return;
    }
    cells_scroll_cols(t, g, top, bot, g->margin_left, (unsigned)g->margin_right + 1, n, up);
}

// A scroll can bring a double-size line under the cursor: the cursor's row
// is touched and the cursor kept inside the line's columns.
static void grid_after_scroll(term_t *t, term_grid_t *g)
{
    term_pos_t *pos = term_pos(g);
    cells_touch(t, g, pos->y);
    unsigned cols = term_cols(g, pos->y);
    if (pos->x >= cols)
        pos->x = (uint8_t)(cols - 1);
}

static bool grid_full_region(const term_grid_t *g)
{
    return g->margin_top == 0 && g->margin_bot == g->height - 1 && grid_all_columns(g);
}

// Index: down a row, scrolling the region at its bottom margin. The core of
// LF, VT, FF, IND and NEL. Below the region the cursor walks down to the last
// row and never scrolls. Outside the left and right margins it stays on the
// bottom margin and nothing scrolls. grid_index_move leaves the cursor's new
// row untouched, for a caller that fills it.
static void grid_index_move(term_t *t, term_grid_t *g)
{
    term_pos_t *pos = term_pos(g);
    pos->wrap_pending = false;
    if (pos->y != g->margin_bot)
    {
        if (pos->y < g->height - 1)
            ++pos->y;
    }
    else if (!grid_in_columns(g, pos->x))
        ;
    else if (grid_full_region(g))
    {
        grid_lines_scroll(g, 0, (uint8_t)(g->height - 1), 1, true);
        cells_index(t, g, true);
    }
    else
        grid_scroll(t, g, g->margin_top, g->margin_bot, 1, true);
}

static void grid_index(term_t *t, term_grid_t *g)
{
    grid_index_move(t, g);
    grid_after_scroll(t, g);
}

// Reverse Index (ESC M), the dual of IND. At margin_top it scrolls the region
// (or whole screen) down by one row; elsewhere the cursor moves up, stopping
// at the top row.
static void grid_reverse_index(term_t *t, term_grid_t *g)
{
    term_pos_t *pos = term_pos(g);
    pos->wrap_pending = false;
    if (pos->y != g->margin_top)
    {
        if (pos->y > 0)
            --pos->y;
    }
    else if (!grid_in_columns(g, pos->x))
        ;
    else if (grid_full_region(g))
    {
        grid_lines_scroll(g, 0, (uint8_t)(g->height - 1), 1, false);
        cells_index(t, g, false);
    }
    else
        grid_scroll(t, g, g->margin_top, g->margin_bot, 1, false);
    grid_after_scroll(t, g);
}

// Carriage return: to the left margin, or to the first column for a cursor
// a program put left of the margin with DECOM off.
static void grid_CR(term_t *t, term_grid_t *g)
{
    term_pos_t *pos = term_pos(g);
    pos->x = term_pen(t)->origin_mode || pos->x >= g->margin_left ? g->margin_left : 0;
    pos->wrap_pending = false;
}

// LF, VT and FF: an index, and a carriage return too under LNM.
static void grid_LF(term_t *t, term_grid_t *g)
{
    grid_index(t, g);
    if (t->newline_mode)
        grid_CR(t, g);
}

/* ---- glyphs ------------------------------------------------------------ */

// A glyph is about to be written: a pending wrap goes to the left margin of
// the next row first, with DECAWM on.
static void grid_wrap(term_t *t, term_grid_t *g)
{
    term_pos_t *pos = term_pos(g);
    if (!pos->wrap_pending)
        return;
    pos->wrap_pending = false;
    if (t->autowrap)
    {
        grid_index(t, g);
        pos->x = g->margin_left;
    }
}

// True when glyphs insert. A VT52 has no insert mode, so IRM is suspended in
// VT52 mode and takes effect again in ANSI mode, as in xterm.
static bool grid_inserting(const term_t *t)
{
    return t->insert_mode && !t->vt52;
}

// Write one glyph at the cursor and move on. Writing the last column, or the
// right margin, leaves the cursor there with a wrap pending (DECAWM on), or
// just stays there (DECAWM off). In insert mode (IRM) the rest of the row
// up to the right margin moves right first, and what passes it is lost.
static void grid_put_glyph(term_t *t, term_grid_t *g, uint8_t byte)
{
    term_pos_t *pos = term_pos(g);
    grid_wrap(t, g);
    unsigned stop = grid_right_stop(g);
    if (grid_inserting(t) && grid_in_columns(g, pos->x))
        cells_insert(t, g, pos->y, pos->x, stop + 1, 1);
    cells_glyphs(t, g, pos->y, pos->x, byte, 1);
    if (pos->x < stop)
        pos->x++;
    else if (t->autowrap)
        pos->wrap_pending = true;
}

// One pass of a REP between left and right margins. The cursor goes where
// the n glyphs take it, and the scrolls of the region on the way are
// counted, not done: `done` is how many so far. With `write`, the glyphs
// are written where they are once all `total` scrolls are done: a run
// between the margins of a row in the region is that many rows higher for
// each scroll still to come, or gone off the top, and anything else stays
// on its row.
static unsigned grid_rep_pass(term_t *t, term_grid_t *g, unsigned n, bool write,
                              unsigned total)
{
    term_pos_t *pos = term_pos(g);
    unsigned done = 0;
    while (n)
    {
        if (pos->wrap_pending)
        {
            pos->wrap_pending = false;
            if (t->autowrap)
            {
                if (pos->y != g->margin_bot)
                {
                    if (pos->y < g->height - 1)
                        ++pos->y;
                }
                else if (grid_in_columns(g, pos->x))
                    done++;
                pos->x = g->margin_left;
            }
        }
        unsigned cols = term_cols(g, pos->y);
        if (pos->x >= cols)
            pos->x = (uint8_t)(cols - 1);
        unsigned stop = grid_right_stop(g);
        unsigned run = (unsigned)(stop + 1 - pos->x);
        if (run > n)
            run = n;
        if (write)
        {
            // The run's cells outside the margins, which are left of them
            // or all of it, then those between.
            unsigned x = pos->x, end = x + run;
            unsigned out = x > g->margin_right ? end : x < g->margin_left ? g->margin_left : x;
            if (out > end)
                out = end;
            if (out > x)
            {
                cells_touch(t, g, pos->y);
                cells_glyphs(t, g, pos->y, (uint8_t)x, t->last_glyph, out - x);
            }
            int y = pos->y;
            if (y >= g->margin_top && y <= g->margin_bot)
                y -= (int)(total - done);
            if (end > out && (y >= g->margin_top || pos->y < g->margin_top))
            {
                cells_touch(t, g, (uint8_t)y);
                cells_glyphs(t, g, (uint8_t)y, (uint8_t)out, t->last_glyph, end - out);
            }
        }
        n -= run;
        if (pos->x + run <= stop)
            pos->x = (uint8_t)(pos->x + run);
        else
        {
            pos->x = (uint8_t)stop;
            pos->wrap_pending = t->autowrap;
            if (!t->autowrap)
                n = 0;
        }
    }
    return done;
}

// REP, CSI Pn b: the last graphic character again, Pn times (xterm,
// ECMA-48). After a row more than the grid holds, every row the glyphs can
// reach is full of the glyph, the scrollback included, and each further run
// of whole rows changes nothing; so a long count runs only that far, plus its
// remainder. A run is a multiple of both a row's length and a double-size
// row's, which is just the width when the width is even. The glyphs go a
// row at a time, and a whole row with more to come is only owed its glyphs,
// so a long REP writes the row it starts on and the row it ends on.
//
// Between left and right margins a run is the columns between them, after
// the row the cursor starts on, which can be as long as the width. There a
// scroll moves cells, so the scrolls are counted first and done as one, and
// then the glyphs are written where that leaves them.
static void grid_REP(term_t *t, term_grid_t *g)
{
    unsigned n = grid_param(t, 0, 1);
    unsigned run = (g->width & 1) && g->width > 1 ? g->width * (g->width / 2u) : g->width;
    unsigned most = run * (g->rows + 1u);
    if (!grid_all_columns(g))
    {
        run = (unsigned)g->margin_right + 1 - g->margin_left;
        most = g->width + run * (g->rows + 1u);
    }
    if (n > most)
        n = most + (n - most) % run;
    if (grid_inserting(t))
    {
        while (n--)
            grid_put_glyph(t, g, t->last_glyph);
        return;
    }
    term_pos_t *pos = term_pos(g);
    if (!grid_all_columns(g))
    {
        const term_pos_t from = *pos;
        const unsigned scrolls = grid_rep_pass(t, g, n, false, 0);
        *pos = from;
        grid_scroll(t, g, g->margin_top, g->margin_bot, (uint16_t)scrolls, true);
        grid_rep_pass(t, g, n, true, scrolls);
        cells_touch(t, g, pos->y);
        return;
    }
    while (n)
    {
        // A pending wrap goes to the next row, as grid_wrap does, but the
        // row is not touched until it is known what it gets.
        bool moved = false;
        if (pos->wrap_pending)
        {
            pos->wrap_pending = false;
            if (t->autowrap)
            {
                grid_index_move(t, g);
                pos->x = g->margin_left;
                moved = true;
            }
        }
        unsigned cols = term_cols(g, pos->y);
        if (pos->x >= cols)
            pos->x = (uint8_t)(cols - 1);
        unsigned run = (unsigned)(cols - pos->x);
        if (run > n)
            run = n;
        if (run == g->width && n > run && t->autowrap)
            // A whole row, with more to come: the row is owed the glyph,
            // whatever it was owed, and the cursor goes on from it. Not a
            // double-size row, whose far half keeps what it holds.
            cells_fill_row(t, g, pos->y, t->last_glyph);
        else
        {
            if (moved)
                cells_touch(t, g, pos->y);
            cells_glyphs(t, g, pos->y, pos->x, t->last_glyph, run);
        }
        n -= run;
        if (pos->x + run < cols)
            pos->x = (uint8_t)(pos->x + run);
        else
        {
            // The last column: what is left wraps, or with DECAWM off lands
            // on that column again, which already holds the glyph.
            pos->x = (uint8_t)(cols - 1);
            pos->wrap_pending = t->autowrap;
            if (!t->autowrap)
                n = 0;
        }
    }
}

/* ---- tabs -------------------------------------------------------------- */

static inline bool grid_tab_is_set(const term_grid_t *g, uint8_t col)
{
    return (g->tab_stops[col >> 3] & (1u << (col & 7))) != 0;
}

// Forward to the next tab stop, or the last column or the right margin when
// there is none (Linux and xterm: HT stops at the edge, it doesn't wrap). A
// pending wrap stays pending, as in Linux and xterm: the cursor is already
// at the edge.
static void grid_tab_forward(term_grid_t *g, uint16_t n)
{
    term_pos_t *pos = term_pos(g);
    unsigned last = grid_right_stop(g);
    while (n-- && pos->x < last)
    {
        do
            pos->x++;
        while (pos->x < last && !grid_tab_is_set(g, pos->x));
    }
}

// Back to the previous tab stop, or the first column when there is none:
// the left margin under DECOM, as on xterm.
static void grid_tab_back(term_t *t, term_grid_t *g, uint16_t n)
{
    term_pos_t *pos = term_pos(g);
    unsigned first = term_pen(t)->origin_mode ? g->margin_left : 0;
    pos->wrap_pending = false;
    while (n-- && pos->x > first)
    {
        do
            pos->x--;
        while (pos->x > first && !grid_tab_is_set(g, pos->x));
    }
}

// Tab Clear. Ps=0 (default) clears the stop at the cursor; Ps=3 clears all.
static void grid_TBC(term_t *t, term_grid_t *g)
{
    uint16_t ps = t->csi_param[0];
    uint8_t x = term_pos(g)->x;
    if (ps == 3)
        memset(g->tab_stops, 0, sizeof(g->tab_stops));
    else if (ps == 0)
        g->tab_stops[x >> 3] &= (uint8_t)~(1u << (x & 7));
}

/* ---- cursor movement --------------------------------------------------- */

// Up n rows. A cursor inside the region stops at its top margin; above it, at
// the top row.
static void grid_cursor_up(term_t *t, term_grid_t *g, uint16_t n)
{
    term_pos_t *pos = term_pos(g);
    uint8_t fence = (pos->y >= g->margin_top) ? g->margin_top : 0;
    uint16_t room = (uint16_t)(pos->y - fence);
    grid_goto(t, g, pos->x, (unsigned)(pos->y - (n < room ? n : room)));
}

// Down n rows. A cursor inside the region stops at its bottom margin; below
// it, at the last row.
static void grid_cursor_down(term_t *t, term_grid_t *g, uint16_t n)
{
    term_pos_t *pos = term_pos(g);
    uint8_t fence = (pos->y <= g->margin_bot) ? g->margin_bot
                                              : (uint8_t)(g->height - 1);
    uint16_t room = (uint16_t)(fence - pos->y);
    grid_goto(t, g, pos->x, (unsigned)(pos->y + (n < room ? n : room)));
}

// Right n columns: a cursor at or left of the right margin stops at it.
static void grid_cursor_forward(term_t *t, term_grid_t *g, uint16_t n)
{
    term_pos_t *pos = term_pos(g);
    unsigned stop = grid_right_stop(g);
    unsigned room = stop - (pos->x < stop ? pos->x : stop);
    grid_goto(t, g, (unsigned)pos->x + (n < room ? n : room), pos->y);
}

// Left n columns: a cursor at or right of the left margin stops at it.
static void grid_cursor_back(term_t *t, term_grid_t *g, uint16_t n)
{
    term_pos_t *pos = term_pos(g);
    unsigned room = pos->x - grid_left_stop(g);
    grid_goto(t, g, (unsigned)pos->x - (n < room ? n : room), pos->y);
}

// Translate a 1-indexed row from a CSI parameter into an absolute 0-indexed
// row, honoring DECOM. In origin mode the row is clamped inside the scroll
// region; otherwise grid_goto clamps it to the screen.
static unsigned grid_origin_row(term_t *t, const term_grid_t *g, uint16_t row)
{
    if (!term_pen(t)->origin_mode)
        return (unsigned)row - 1u;
    unsigned region_h = (unsigned)(g->margin_bot - g->margin_top + 1);
    if (row > region_h)
        row = (uint16_t)region_h;
    return (unsigned)g->margin_top + row - 1u;
}

// The same for a column: under DECOM it counts from the left margin and
// stops at the right one.
static unsigned grid_origin_col(term_t *t, const term_grid_t *g, uint16_t col)
{
    if (!term_pen(t)->origin_mode)
        return (unsigned)col - 1u;
    unsigned w = (unsigned)(g->margin_right - g->margin_left + 1);
    if (col > w)
        col = (uint16_t)w;
    return (unsigned)g->margin_left + col - 1u;
}

// Home: the top left, or the margins' under DECOM.
static void grid_home(term_t *t, term_grid_t *g)
{
    bool origin = term_pen(t)->origin_mode;
    grid_goto(t, g, origin ? g->margin_left : 0, origin ? g->margin_top : 0);
}

/* ---- erasing and editing ----------------------------------------------- */

// Erase Line: 0 cursor to end, 1 start to cursor, 2 the whole line. A
// selective erase leaves the protected cells.
static void grid_erase_line(term_t *t, term_grid_t *g, uint16_t mode, bool selective)
{
    term_pos_t *pos = term_pos(g);
    pos->wrap_pending = false;
    unsigned from = 0, to = term_cols(g, pos->y);
    if (mode == 0)
        from = pos->x;
    else if (mode == 1)
        to = (unsigned)pos->x + 1u;
    else if (mode != 2)
        return;
    cells_erase(t, g, pos->y, from, to, selective);
}

// Lazy-erase rows [from, to), which become single size unless the erase is
// selective: a row whose protected cells stay keeps its size, as on xterm.
static void grid_erase_rows(term_t *t, term_grid_t *g, uint8_t from, uint8_t to,
                            bool selective)
{
    if (!selective)
        grid_lines_normal(g, from, to);
    cells_erase_rows(t, g, from, to, selective);
}

// Erase Display. ED 0 and ED 1 erase the cursor's row like EL 0 and EL 1 and
// lazy-erase the rows past it. ED 2 lazy-erases every row and touches the
// cursor's. ED 3 (xterm) forgets the scrollback: the rows a taller view would
// bring back.
static void grid_erase_display(term_t *t, term_grid_t *g, uint16_t mode, bool selective)
{
    term_pos_t *pos = term_pos(g);
    switch (mode)
    {
    case 0:
        grid_erase_rows(t, g, (uint8_t)(pos->y + 1), g->height, selective);
        grid_erase_line(t, g, 0, selective);
        break;
    case 1:
        grid_erase_rows(t, g, 0, pos->y, selective);
        grid_erase_line(t, g, 1, selective);
        break;
    case 2:
        pos->wrap_pending = false;
        grid_erase_rows(t, g, 0, g->height, selective);
        cells_touch(t, g, pos->y);
        break;
    case 3:
        pos->wrap_pending = false;
        cells_forget(t, g);
        break;
    }
}

// The columns from the cursor up to end, and Pn of them at most: what DCH,
// ICH and ECH act on. A pending wrap ends.
static unsigned grid_edit_count(term_t *t, term_grid_t *g, unsigned end)
{
    term_pos_t *pos = term_pos(g);
    pos->wrap_pending = false;
    unsigned avail = end - pos->x;
    unsigned n = grid_param(t, 0, 1);
    return n > avail ? avail : n;
}

// Whether the cursor is inside the scroll region and between the left and
// right margins: what the sequences that move part of the screen need.
static bool grid_cursor_in_margins(const term_grid_t *g)
{
    const term_pos_t *pos = term_pos(g);
    return pos->y >= g->margin_top && pos->y <= g->margin_bot && grid_in_columns(g, pos->x);
}

// Insert or delete Pn lines at the cursor, inside the margins only; the
// cursor goes to the left margin (xterm).
static void grid_IL_DL(term_t *t, term_grid_t *g, bool insert)
{
    uint8_t y = term_pos(g)->y;
    if (!grid_cursor_in_margins(g))
        return;
    grid_scroll(t, g, y, g->margin_bot, grid_param(t, 0, 1), !insert);
    grid_goto(t, g, g->margin_left, y);
}

// Scroll the region up (SU) or down (SD) by Pn rows. The cursor stays.
static void grid_SU_SD(term_t *t, term_grid_t *g, bool up)
{
    grid_scroll(t, g, g->margin_top, g->margin_bot, grid_param(t, 0, 1), up);
    grid_after_scroll(t, g);
}

// Set Top and Bottom Margins (CSI Pt ; Pb r). The cursor goes home. Each
// grid clamps them to its own height.
static void grid_DECSTBM(term_t *t, term_grid_t *g)
{
    uint16_t top = grid_param(t, 0, 1);
    uint16_t bot = grid_param(t, 1, g->height);
    if (bot > g->height)
        bot = g->height;
    if (top >= bot)
        return; // invalid region: ignore (VT100)
    g->margin_top = (uint8_t)(top - 1);
    g->margin_bot = (uint8_t)(bot - 1);
    grid_home(t, g);
}

// Set Left and Right Margins (CSI Pl ; Pr s, with DECLRMM set). The cursor
// goes home. Each grid clamps them to its own width.
static void grid_DECSLRM(term_t *t, term_grid_t *g)
{
    uint16_t left = grid_param(t, 0, 1);
    uint16_t right = grid_param(t, 1, g->width);
    if (right > g->width)
        right = g->width;
    if (left >= right)
        return;
    g->margin_left = (uint8_t)(left - 1);
    g->margin_right = (uint8_t)(right - 1);
    grid_home(t, g);
}

// DECDHL, DECDWL and DECSWL, ESC # 3 to 6: the size of the cursor's line,
// at once, text already on it included. Going double size, the columns past
// the middle are lost; DEC blanks them.
static void grid_line_size(term_t *t, term_grid_t *g, uint8_t size)
{
    term_pos_t *pos = term_pos(g);
    uint8_t *line = &term_lines(g)[pos->y];
    if (size != TERM_LINE_NORMAL && *line == TERM_LINE_NORMAL)
        cells_erase(t, g, pos->y, g->width / 2u, g->width, false);
    *line = size;
    grid_goto(t, g, pos->x, pos->y);
}

// DECALN, ESC # 8: the screen full of E's, for lining up a monitor. The
// margins reset, every line is single size, and the cursor goes home. The E's
// come in as a lazy erase does, a row a tick.
static void grid_DECALN(term_t *t, term_grid_t *g)
{
    grid_full_margins(g);
    grid_lines_normal(g, 0, g->height);
    cells_align(t, g);
    grid_goto(t, g, 0, 0);
}

/* ---- the sequences ----------------------------------------------------- */

// The CSI sequences that move the cursor or the rows.
static void grid_csi(term_t *t, term_grid_t *g, uint8_t ch)
{
    term_pos_t *pos = term_pos(g);
    unsigned n, end;
    switch (ch)
    {
    case '@': // ICH: Pn erased cells at the cursor, up to the right margin
        if (!grid_in_columns(g, pos->x))
            break;
        end = grid_right_stop(g) + 1;
        n = grid_edit_count(t, g, end);
        cells_insert(t, g, pos->y, pos->x, end, n);
        break;
    case 'A':
        grid_cursor_up(t, g, grid_param(t, 0, 1));
        break;
    case 'B':
    case 'e': // VPR -- alias of CUD
        grid_cursor_down(t, g, grid_param(t, 0, 1));
        break;
    case 'C':
    case 'a': // HPR -- alias of CUF
        grid_cursor_forward(t, g, grid_param(t, 0, 1));
        break;
    case 'D':
        grid_cursor_back(t, g, grid_param(t, 0, 1));
        break;
    case 'E': // CNL
        grid_cursor_down(t, g, grid_param(t, 0, 1));
        grid_CR(t, g);
        break;
    case 'F': // CPL
        grid_cursor_up(t, g, grid_param(t, 0, 1));
        grid_CR(t, g);
        break;
    case 'G':
    case '`': // HPA -- alias of CHA
        grid_goto(t, g, grid_origin_col(t, g, grid_param(t, 0, 1)), pos->y);
        break;
    case 'H':
    case 'f': // HVP -- alias of CUP
        grid_goto(t, g, grid_origin_col(t, g, grid_param(t, 1, 1)),
                  grid_origin_row(t, g, grid_param(t, 0, 1)));
        break;
    case 'I': // CHT
        grid_tab_forward(g, grid_param(t, 0, 1));
        break;
    case 'J':
        grid_erase_display(t, g, t->csi_param[0], term_erase_selective(t, false));
        break;
    case 'K':
        grid_erase_line(t, g, t->csi_param[0], term_erase_selective(t, false));
        break;
    case 'L':
        grid_IL_DL(t, g, true);
        break;
    case 'M':
        grid_IL_DL(t, g, false);
        break;
    case 'P': // DCH: the rest of the row up to the right margin moves left
        if (!grid_in_columns(g, pos->x))
            break;
        end = grid_right_stop(g) + 1;
        n = grid_edit_count(t, g, end);
        cells_delete(t, g, pos->y, pos->x, end, n);
        break;
    case 'S':
        grid_SU_SD(t, g, true);
        break;
    case 'T':
        grid_SU_SD(t, g, false);
        break;
    case 'X': // ECH: Pn cells erased; the cursor doesn't move
        n = grid_edit_count(t, g, term_cols(g, pos->y));
        cells_erase(t, g, pos->y, pos->x, pos->x + n, term_erase_selective(t, false));
        break;
    case 'Z': // CBT
        grid_tab_back(t, g, grid_param(t, 0, 1));
        break;
    case 'd': // VPA
        grid_goto(t, g, pos->x, grid_origin_row(t, g, grid_param(t, 0, 1)));
        break;
    case 'g':
        grid_TBC(t, g);
        break;
    case 'r':
        grid_DECSTBM(t, g);
        break;
    case 's':
        grid_DECSLRM(t, g);
        break;
    }
}

// VT52's cursor and erase sequences.
static void grid_vt52(term_t *t, term_grid_t *g, uint8_t ch)
{
    switch (ch)
    {
    case 'A': // cursor up
        grid_cursor_up(t, g, 1);
        break;
    case 'B': // cursor down
        grid_cursor_down(t, g, 1);
        break;
    case 'C': // cursor right
        grid_cursor_forward(t, g, 1);
        break;
    case 'D': // cursor left
        grid_cursor_back(t, g, 1);
        break;
    case 'H': // home
        grid_goto(t, g, 0, 0);
        break;
    case 'I': // reverse line feed
        grid_reverse_index(t, g);
        break;
    case 'J': // erase to end of screen
        grid_erase_display(t, g, 0, term_erase_selective(t, false));
        break;
    case 'K': // erase to end of line
        grid_erase_line(t, g, 0, term_erase_selective(t, false));
        break;
    }
}

/* ---- rectangles -------------------------------------------------------- */

// The VT420's rectangle sequences. A rectangle is its top, left, bottom and
// right, counted from 1, and under DECOM from the margins, which it then
// stays inside. A side left out is the edge of the screen, or of the margins
// under DECOM, and a side past the edge is the edge, as on xterm. A
// double-size line has only its own columns of a rectangle.
typedef struct
{
    uint8_t top, left, bot, right; // inclusive, from 0
} grid_rect_t;

// One side of a rectangle: parameter i when it is one of the first `count`
// and not left out, otherwise dflt. It counts from min and stays in
// [min, max].
static uint8_t grid_rect_side(const term_t *t, unsigned i, unsigned count,
                              unsigned dflt, unsigned min, unsigned max)
{
    unsigned v = i < count && i < t->csi_param_count ? t->csi_param[i] : 0;
    if (!v)
        return (uint8_t)dflt;
    v = v - 1 + min;
    return (uint8_t)(v > max ? max : v);
}

// The rectangle whose sides are parameters i to i + 3, of which only those
// before `count` are read. False when nothing is inside it.
static bool grid_rect_parse(term_t *t, const term_grid_t *g, unsigned i, unsigned count,
                            grid_rect_t *r)
{
    bool origin = term_pen(t)->origin_mode;
    unsigned y0 = origin ? g->margin_top : 0, y1 = origin ? g->margin_bot : g->height - 1u;
    unsigned x0 = origin ? g->margin_left : 0, x1 = origin ? g->margin_right : g->width - 1u;
    r->top = grid_rect_side(t, i, count, y0, y0, y1);
    r->left = grid_rect_side(t, i + 1, count, x0, x0, x1);
    r->bot = grid_rect_side(t, i + 2, count, y1, y0, y1);
    r->right = grid_rect_side(t, i + 3, count, x1, x0, x1);
    return r->top <= r->bot && r->left <= r->right;
}

// The columns of a rectangle on row y, as [r->left, end): fewer on a
// double-size line.
static unsigned grid_rect_end(const term_grid_t *g, const grid_rect_t *r, uint8_t y)
{
    unsigned cols = term_cols(g, y);
    return r->right < cols ? r->right + 1u : cols;
}

// A whole row that the cursor is not on can be owed what a rectangle does
// to it.
static bool grid_rect_whole_row(const term_grid_t *g, const grid_rect_t *r, uint8_t y,
                                unsigned end)
{
    return r->left == 0 && end == g->width && y != term_pos(g)->y;
}

// DECFRA, CSI Pch ; Pt ; Pl ; Pb ; Pr $ x: the rectangle full of a
// character, as the pen writes it. Any byte that prints is taken.
static void grid_DECFRA(term_t *t, term_grid_t *g)
{
    unsigned ch = t->csi_param[0] ? t->csi_param[0] : ' ';
    grid_rect_t r;
    if (ch < 0x20 || ch == 0x7F || ch > 0xFF || !grid_rect_parse(t, g, 1, 5, &r))
        return;
    for (unsigned y = r.top; y <= r.bot; y++)
    {
        unsigned end = grid_rect_end(g, &r, (uint8_t)y);
        if (r.left >= end)
            continue;
        if (grid_rect_whole_row(g, &r, (uint8_t)y, end))
            cells_fill_row(t, g, (uint8_t)y, (uint8_t)ch);
        else
        {
            cells_touch(t, g, (uint8_t)y);
            cells_glyphs(t, g, (uint8_t)y, r.left, (uint8_t)ch, end - r.left);
        }
    }
}

// DECERA, CSI Pt ; Pl ; Pb ; Pr $ z, erases the rectangle as ED erases the
// screen, protected cells too. DECSERA, the same with $ {, leaves them, as
// DECSED does. Neither changes a line's size.
static void grid_rect_erase(term_t *t, term_grid_t *g, bool selective)
{
    grid_rect_t r;
    if (!grid_rect_parse(t, g, 0, 4, &r))
        return;
    for (unsigned y = r.top; y <= r.bot; y++)
    {
        unsigned end = grid_rect_end(g, &r, (uint8_t)y);
        if (r.left >= end)
            continue;
        if (grid_rect_whole_row(g, &r, (uint8_t)y, end))
            cells_erase_rows(t, g, (uint8_t)y, (uint8_t)(y + 1), selective);
        else
            grid_erase_cols(t, g, (uint8_t)y, r.left, end, selective);
    }
}

// DECCRA, CSI Pts ; Pls ; Pbs ; Prs ; Pps ; Ptd ; Pld ; Ppd $ v: the
// rectangle copied to another place, cells, colors, attributes and all.
// There is one page, so the page numbers are not read. What would land
// past the edge is lost.
static void grid_DECCRA(term_t *t, term_grid_t *g)
{
    grid_rect_t s, d;
    if (!grid_rect_parse(t, g, 0, 4, &s) || !grid_rect_parse(t, g, 5, 7, &d))
        return;
    unsigned h = (unsigned)s.bot - s.top + 1, w = (unsigned)s.right - s.left + 1;
    if (h > (unsigned)d.bot - d.top + 1)
        h = (unsigned)d.bot - d.top + 1;
    if (w > (unsigned)d.right - d.left + 1)
        w = (unsigned)d.right - d.left + 1;
    grid_copy(t, g, s.top, s.left, d.top, d.left, h, w);
}

// DECCARA, CSI Pt ; Pl ; Pb ; Pr ; Ps ... $ r, changes the attributes of
// the cells already there: 1, 4, 5 and 7 set bold, underline, blink and
// reverse, 22, 24, 25 and 27 clear them, and 0 clears all four. DECRARA,
// the same with $ t, reverses 1, 4, 5 and 7, or all four with 0. With
// DECSACE 2 they change the rectangle. Without, they change the stream of
// cells from its top left to its bottom right: the rows between those two
// are changed from the first column to the last.
static void grid_rect_attrs(term_t *t, term_grid_t *g, bool reverse)
{
    grid_rect_t r;
    if (!grid_rect_parse(t, g, 0, 4, &r))
        return;
    unsigned set = 0, clear = 0, toggle = 0;
    unsigned i = 4;
    do
    {
        unsigned p = i < t->csi_param_count ? t->csi_param[i] : 0;
        unsigned bit = 0;
        switch (p)
        {
        case 0:
            bit = CELLS_MARK_ALL;
            break;
        case 1:
        case 22:
            bit = CELLS_MARK_BOLD;
            break;
        case 4:
        case 24:
            bit = CELLS_MARK_UNDERLINE;
            break;
        case 5:
        case 25:
            bit = CELLS_MARK_BLINK;
            break;
        case 7:
        case 27:
            bit = CELLS_MARK_REVERSE;
            break;
        }
        if (reverse)
            toggle ^= p < 20 ? bit : 0;
        else if (p == 0 || p >= 20)
        {
            clear |= bit;
            set &= ~bit;
        }
        else
        {
            set |= bit;
            clear &= ~bit;
        }
    } while (++i < t->csi_param_count);
    for (unsigned y = r.top; y <= r.bot; y++)
    {
        grid_rect_t row = r;
        if (!t->rect_attrs)
        {
            if (y != r.top)
                row.left = 0;
            if (y != r.bot)
                row.right = (uint8_t)(g->width - 1);
        }
        unsigned end = grid_rect_end(g, &row, (uint8_t)y);
        if (row.left >= end)
            continue;
        cells_attrs(t, g, (uint8_t)y, row.left, end, set, clear, toggle);
    }
}

static void grid_rect(term_t *t, term_grid_t *g, uint8_t ch)
{
    switch (ch)
    {
    case 'x':
        grid_DECFRA(t, g);
        break;
    case 'z':
        grid_rect_erase(t, g, false);
        break;
    case '{':
        grid_rect_erase(t, g, term_erase_selective(t, true));
        break;
    case 'v':
        grid_DECCRA(t, g);
        break;
    case 'r':
        grid_rect_attrs(t, g, false);
        break;
    case 't':
        grid_rect_attrs(t, g, true);
        break;
    }
}

void grid_do(term_t *t, term_grid_t *g, grid_op_t op, uint8_t ch)
{
    term_pos_t *pos = term_pos(g);
    switch (op)
    {
    case GRID_PRINT:
        grid_put_glyph(t, g, ch);
        break;
    case GRID_C0:
        if (ch == '\b')
        {
            pos->wrap_pending = false;
            if (pos->x > grid_left_stop(g))
                pos->x--;
        }
        else if (ch == '\t')
            grid_tab_forward(g, 1);
        else if (ch == '\r')
            grid_CR(t, g);
        else // LF, VT and FF: all three are LF, as on a VT102 and in xterm and Linux
            grid_LF(t, g);
        break;
    case GRID_CSI:
        grid_csi(t, g, ch);
        break;
    case GRID_REP:
        grid_REP(t, g);
        break;
    case GRID_SELECTIVE:
        // DECSED and DECSEL; DECSED 3, xterm's selective erase of the
        // scrollback, has none to spare cells in, and is ignored.
        if (ch == 'J' && t->csi_param[0] != 3)
            grid_erase_display(t, g, t->csi_param[0], term_erase_selective(t, true));
        else if (ch == 'K')
            grid_erase_line(t, g, t->csi_param[0], term_erase_selective(t, true));
        break;
    case GRID_SHIFT:
        // SL and SR, CSI Pn SP @ and CSI Pn SP A: every line of the scroll
        // region moves Pn columns between the left and right margins, for
        // a cursor inside them, as on xterm. The cursor stays.
        if (grid_cursor_in_margins(g))
            cells_shift(t, g, g->margin_top, g->margin_bot, g->margin_left,
                        (unsigned)g->margin_right + 1, grid_param(t, 0, 1), ch == '@');
        break;
    case GRID_COLUMNS:
        // DECIC and DECDC, CSI Pn ' } and CSI Pn ' ~: Pn columns inserted
        // or deleted at the cursor's column, on every line of the scroll
        // region, up to the right margin. For a cursor inside the margins.
        if (grid_cursor_in_margins(g))
            cells_shift(t, g, g->margin_top, g->margin_bot, pos->x,
                        (unsigned)g->margin_right + 1, grid_param(t, 0, 1), ch == '~');
        break;
    case GRID_RECT:
        grid_rect(t, g, ch);
        break;
    case GRID_LRMM:
        // DECLRMM: set, every line is single size, as on a VT420 and
        // xterm; reset, the left and right margins are the whole width.
        if (ch)
            memset(g->lines, TERM_LINE_NORMAL, 2u * g->rows);
        else
            grid_full_columns(g);
        break;
    case GRID_ESC:
        if (ch == 'D') // IND
            grid_index(t, g);
        else if (ch == 'E') // NEL
        {
            grid_CR(t, g);
            grid_index(t, g);
        }
        else if (ch == 'H') // HTS, a tab stop at the cursor
            g->tab_stops[pos->x >> 3] |= (uint8_t)(1u << (pos->x & 7));
        else if (ch == '6' || ch == '9')
        {
            // DECBI and DECFI: a column left or right, and at the left or
            // right margin the columns between the margins move the other
            // way instead, on every line of the scroll region, as xterm.
            const bool back = ch == '6';
            if (grid_in_columns(g, pos->x) && pos->x == (back ? g->margin_left : g->margin_right))
            {
                if (grid_cursor_in_margins(g))
                    cells_shift(t, g, g->margin_top, g->margin_bot, g->margin_left,
                                (unsigned)g->margin_right + 1, 1, !back);
            }
            else if (back)
                grid_cursor_back(t, g, 1);
            else
                grid_cursor_forward(t, g, 1);
        }
        else // RI
            grid_reverse_index(t, g);
        break;
    case GRID_LINE:
        if (ch == '8')
            grid_DECALN(t, g);
        else
            grid_line_size(t, g, ch == '3'   ? TERM_LINE_TOP
                                 : ch == '4' ? TERM_LINE_BOTTOM
                                 : ch == '6' ? TERM_LINE_WIDE
                                             : TERM_LINE_NORMAL);
        break;
    case GRID_VT52:
        grid_vt52(t, g, ch);
        break;
    case GRID_VT52_Y:
    {
        // A row off the screen leaves the cursor's row alone; a column off it
        // goes to the last column.
        unsigned row = (unsigned)(t->csi_param[0] - 32);
        if (row >= g->height)
            row = pos->y;
        grid_goto(t, g, (unsigned)(ch - 32), row);
        break;
    }
    case GRID_SAVE:
        g->saved[t->alt_active] = *pos;
        break;
    case GRID_RESTORE:
    {
        // With nothing saved the slot holds home.
        *pos = g->saved[t->alt_active];
        bool pending = pos->wrap_pending;
        grid_goto(t, g, pos->x, pos->y);
        pos->wrap_pending = pending;
        break;
    }
    case GRID_HOME:
        grid_home(t, g);
        break;
    case GRID_STAY:
        grid_goto(t, g, pos->x, pos->y);
        break;
    case GRID_ALT_IN:
        g->pos[1] = g->pos[0];
        break;
    case GRID_ALT_OUT:
        g->pos[0] = g->pos[1];
        break;
    case GRID_CLEAR:
        grid_lines_normal(g, 0, g->rows);
        cells_clear(t, g);
        break;
    case GRID_RESET:
        grid_full_margins(g);
        grid_reset_tabs(g);
        for (int s = 0; s < 2; s++)
        {
            g->pos[s].wrap_pending = false;
            memset(&g->saved[s], 0, sizeof g->saved[s]);
        }
        memset(g->lines + g->rows, TERM_LINE_NORMAL, g->rows);
        cells_reset(t, g);
        break;
    case GRID_DECSTR:
        grid_full_margins(g);
        for (int s = 0; s < 2; s++)
            memset(&g->saved[s], 0, sizeof g->saved[s]);
        grid_goto(t, g, pos->x, pos->y);
        break;
    }
}

/* ---- savestate --------------------------------------------------------- */

static void grid_put_pos(sst_cursor_t *c, const term_pos_t *p)
{
    sst_put_u8(c, p->x);
    sst_put_u8(c, p->y);
    sst_put_bool(c, p->wrap_pending);
}

static void grid_get_pos(sst_cursor_t *c, term_pos_t *p)
{
    p->x = sst_get_u8(c);
    p->y = sst_get_u8(c);
    p->wrap_pending = sst_get_bool(c);
}

void term_grid_sst_put(sst_cursor_t *c, const term_grid_t *g)
{
    sst_put_u8(c, g->width);
    sst_put_u8(c, g->height);
    sst_put_u8(c, g->rows);
    sst_put_u8(c, g->margin_top);
    sst_put_u8(c, g->margin_bot);
    sst_put_u8(c, g->margin_left);
    sst_put_u8(c, g->margin_right);
    for (int s = 0; s < 2; s++)
    {
        grid_put_pos(c, &g->pos[s]);
        grid_put_pos(c, &g->saved[s]);
    }
    sst_put(c, g->tab_stops, TERM_TAB_BITMAP_BYTES);
    sst_put(c, g->lines, 2u * g->rows);
}

/* The grid keeps its rows and lines storage; a blob of another shape is
 * refused. A grid with cells has its width checked by their owner.
 * Everything else is bounded, because it indexes the cells. */
bool term_grid_sst_get(sst_cursor_t *c, term_grid_t *g)
{
    uint8_t w = sst_get_u8(c);
    uint8_t h = sst_get_u8(c);
    uint8_t rows = sst_get_u8(c);
    if (!sst_ok(c) || w < 1 || rows != g->rows || h < 1 || h > rows)
        return false;
    g->width = w;
    g->height = h;
    g->margin_top = sst_get_u8(c);
    g->margin_bot = sst_get_u8(c);
    g->margin_left = sst_get_u8(c);
    g->margin_right = sst_get_u8(c);
    for (int s = 0; s < 2; s++)
    {
        grid_get_pos(c, &g->pos[s]);
        grid_get_pos(c, &g->saved[s]);
    }
    sst_get(c, g->tab_stops, TERM_TAB_BITMAP_BYTES);
    sst_get(c, g->lines, 2u * rows);
    if (!sst_ok(c) || g->margin_top > g->margin_bot || g->margin_bot >= h ||
        g->margin_left > g->margin_right || g->margin_right >= w)
        return false;
    for (int s = 0; s < 2; s++)
        if (g->pos[s].x >= w || g->pos[s].y >= h ||
            g->saved[s].x >= w || g->saved[s].y >= h)
            return false;
    for (unsigned i = 0; i < 2u * rows; i++)
        if (g->lines[i] > TERM_LINE_BOTTOM)
            return false;
    /* The cursor sits inside its own line's columns. */
    for (int s = 0; s < 2; s++)
        if (g->lines[s * rows + g->pos[s].y] && g->pos[s].x >= w / 2)
            return false;
    return true;
}
