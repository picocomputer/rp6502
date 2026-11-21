/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/console_term.h"
#include "core/term/grid.h"
#include "core/term/term.h"
#include "machine.h"
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

/* ---- the cursor -------------------------------------------------------- */

// Move the cursor, 0-indexed, clamped to the screen and to the columns of its
// row. Every move ends a pending wrap, and the destination row is touched so a
// glyph can land on it.
static void grid_goto(term_grid_t *g, unsigned x, unsigned y)
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
    screen_touch(g->shown, (uint8_t)y);
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
static inline HOST_IN_RAM_FUNC bool grid_in_columns(const term_grid_t *g, unsigned x)
{
    return x >= g->margin_left && x <= g->margin_right;
}

// A scroll, an index, a clear or a change of height of the screen showing
// ends the grid's smooth scroll (grid.h).
static inline void grid_cancel(term_grid_t *g)
{
#if TERM_CELLS
    g->cancels++;
#else
    (void)g;
#endif
}

void term_grid_init(term_grid_t *g, uint8_t width, uint8_t *lines, uint8_t *row_idx,
                    uint8_t rows)
{
    memset(g, 0, sizeof *g);
    g->width = width;
    g->height = rows < TERM_STD_HEIGHT ? rows : TERM_STD_HEIGHT;
    for (int s = 0; s < 2; s++)
    {
        screen_init(&g->screen[s], rows, lines, row_idx ? row_idx + s * rows : NULL);
        lines += g->screen[s].mem_rows;
    }
    g->shown = &g->screen[0];
    grid_full_margins(g);
    grid_reset_tabs(g);
}

void term_grid_copy(term_grid_t *g, const term_grid_t *from, uint8_t *lines, uint8_t rows)
{
    *g = *from;
    g->shown = &g->screen[from->shown - from->screen];
    unsigned n = grid_rows(from) < rows ? grid_rows(from) : rows;
    for (int s = 0; s < 2; s++)
    {
        screen_t *sc = &g->screen[s];
        screen_init(sc, rows, lines + s * rows, NULL);
        for (unsigned y = 0; y < n; y++)
            sc->line[y] = from->screen[s].line[screen_mem(&from->screen[s], (uint8_t)y)];
    }
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
        screen_t *sc = &g->screen[s];
        term_pos_t *ps[2] = {&sc->pos, &sc->saved};
        for (int i = 0; i < 2; i++)
        {
            term_pos_t *p = ps[i];
            unsigned cols = screen_size(sc, p->y) ? width / 2u : width;
            if (p->x >= cols)
                p->x = (uint8_t)(cols ? cols - 1 : 0);
            p->wrap_pending = false;
        }
    }
    grid_full_margins(g);
}

/* ---- height ------------------------------------------------------------ */

// The height just went a row up or down: the screen showing moves, and the
// scrollback of each screen is at most what the new height leaves.
static void grid_height_step(term_grid_t *g, bool grow, bool top)
{
    grid_cancel(g);
    screen_height(g->shown, g->height, grow, top);
    for (int s = 0; s < 2; s++)
        screen_fit(&g->screen[s], g->height);
}

void term_set_height(term_t *t, term_grid_t *g, uint8_t height)
{
    (void)t;
    assert(height >= 1 && height <= grid_rows(g));
    term_pos_t *pos = term_pos(g);
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
                grid_height_step(g, true, true);
            }
            else
                // A fresh row at the bottom.
                grid_height_step(g, true, false);
        }
        else
        {
            g->height--;
            if (pos->y == g->height)
            {
                // The cursor is on the bottom row, so the top row goes out of
                // view and joins the scrollback.
                pos->y--;
                grid_height_step(g, false, true);
            }
            else
                // The bottom row is no longer visible.
                grid_height_step(g, false, false);
        }
    }
    grid_full_margins(g);

    // The resize loop only tracks the showing screen's cursor. Any other
    // cursor row (the other screen's and both saved positions) can exceed
    // the new height after a shrink; clamp them so a later swap or DECRC
    // doesn't strand the cursor below the viewport. The showing screen's
    // cursor is inside already.
    uint8_t max_y = (uint8_t)(g->height - 1);
    for (int s = 0; s < 2; s++)
    {
        if (g->screen[s].pos.y > max_y)
            g->screen[s].pos.y = max_y;
        if (g->screen[s].saved.y > max_y)
            g->screen[s].saved.y = max_y;
    }
}

/* ---- scrolling --------------------------------------------------------- */

// Erase columns [from, to) of a row that may still be owed something.
static void grid_erase_cols(term_t *t, term_grid_t *g, uint8_t y, unsigned from,
                            unsigned to, bool selective)
{
    screen_touch(g->shown, y);
    screen_erase(t, g->shown, y, from, to, selective);
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
        screen_copy_cells(t, g->shown, (uint8_t)(sy + k), sx, (uint8_t)(dy + k), dx, n);
    }
}

// Scroll the inclusive region [top, bot] up or down by count rows, erased rows
// coming in. The caller places the cursor and touches its row. Between left
// and right margins only the cells between them move. A scroll of all the
// columns returns the row of memory the row that went out at the edge is
// in, which keep keeps (screen_scroll); between margins nothing is kept, and
// the 0 returned names no row.
static uint8_t grid_scroll(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot,
                           uint16_t count, bool up, bool keep)
{
    if (top > bot || count == 0)
        return 0;
    uint8_t region_h = (uint8_t)(bot - top + 1);
    uint8_t n = count > region_h ? region_h : (uint8_t)count;
    if (grid_all_columns(g))
    {
        grid_cancel(g);
        return screen_scroll(t, g->shown, top, bot, n, up, keep);
    }
    screen_scroll_cols(t, g->shown, top, bot, g->margin_left, (unsigned)g->margin_right + 1, n,
                       up);
    return 0;
}

// A scroll can bring a double-size line under the cursor: the cursor's row
// is touched and the cursor kept inside the line's columns.
static void grid_after_scroll(term_grid_t *g)
{
    term_pos_t *pos = term_pos(g);
    screen_touch(g->shown, pos->y);
    unsigned cols = term_cols(g, pos->y);
    if (pos->x >= cols)
        pos->x = (uint8_t)(cols - 1);
}

static bool grid_full_region(const term_grid_t *g)
{
    return g->margin_top == 0 && g->margin_bot == g->height - 1 && grid_all_columns(g);
}

bool HOST_IN_RAM_FUNC grid_index_scrolls(const term_grid_t *g)
{
    const term_pos_t *pos = term_pos(g);
    return pos->y == g->margin_bot && grid_in_columns(g, pos->x);
}

bool HOST_IN_RAM_FUNC grid_ri_scrolls(const term_grid_t *g)
{
    const term_pos_t *pos = term_pos(g);
    return pos->y == g->margin_top && grid_in_columns(g, pos->x);
}

bool HOST_IN_RAM_FUNC grid_glyph_scrolls(const term_t *t, const term_grid_t *g)
{
    return term_pos(g)->wrap_pending && t->autowrap && grid_index_scrolls(g);
}

// A scroll of one row by an index or a reverse index moves the row a
// scanline a frame under DECSCLM, but one between left and right margins,
// which a VT102 never had, jumps.
static bool grid_smooths(const term_t *t, const term_grid_t *g)
{
    return t->smooth_scroll && grid_all_columns(g);
}

// An index or a reverse index on its margin scrolls the region a row up or
// down. A smooth one pays the row going out what it is owed first, such as
// an erase, and keeps it as it is, because it still shows while it goes;
// then the rows [top, bot] start moving.
static void grid_scroll_one(term_t *t, term_grid_t *g, bool up)
{
    const uint8_t top = g->margin_top, bot = g->margin_bot;
    const uint8_t edge = up ? top : bot;
    const bool keep = grid_smooths(t, g);
    if (keep)
        screen_touch(g->shown, edge);
    uint8_t ghost;
    if (grid_full_region(g))
    {
        grid_cancel(g);
        ghost = screen_index(t, g->shown, g->height, up, keep);
    }
    else
        ghost = grid_scroll(t, g, top, bot, 1, up, keep);
    if (keep)
        console_smooth(t, g, top, bot, up, ghost);
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
    else
        grid_scroll_one(t, g, true);
}

static void grid_index(term_t *t, term_grid_t *g)
{
    grid_index_move(t, g);
    grid_after_scroll(g);
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
    else
        grid_scroll_one(t, g, false);
    grid_after_scroll(g);
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
    unsigned stop = term_right_stop(g);
    if (grid_inserting(t) && grid_in_columns(g, pos->x))
        screen_insert(t, g->shown, pos->y, pos->x, stop + 1, 1);
    screen_glyphs(t, g->shown, pos->y, pos->x, byte, 1);
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
        unsigned stop = term_right_stop(g);
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
                screen_touch(g->shown, pos->y);
                screen_glyphs(t, g->shown, pos->y, (uint8_t)x, t->last_glyph, out - x);
            }
            int y = pos->y;
            if (y >= g->margin_top && y <= g->margin_bot)
                y -= (int)(total - done);
            if (end > out && (y >= g->margin_top || pos->y < g->margin_top))
            {
                screen_touch(g->shown, (uint8_t)y);
                screen_glyphs(t, g->shown, (uint8_t)y, (uint8_t)out, t->last_glyph, end - out);
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
    unsigned most = run * (grid_rows(g) + 1u);
    if (!grid_all_columns(g))
    {
        run = (unsigned)g->margin_right + 1 - g->margin_left;
        most = g->width + run * (grid_rows(g) + 1u);
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
        grid_scroll(t, g, g->margin_top, g->margin_bot, (uint16_t)scrolls, true, false);
        grid_rep_pass(t, g, n, true, scrolls);
        screen_touch(g->shown, pos->y);
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
            screen_fill_row(t, g->shown, pos->y, t->last_glyph);
        else
        {
            if (moved)
                screen_touch(g->shown, pos->y);
            screen_glyphs(t, g->shown, pos->y, pos->x, t->last_glyph, run);
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

// Forward to the next tab stop, or the last column or the right margin when
// there is none (Linux and xterm: HT stops at the edge, it doesn't wrap). A
// pending wrap stays pending, as in Linux and xterm: the cursor is already
// at the edge.
static void grid_tab_forward(term_grid_t *g, uint16_t n)
{
    term_pos_t *pos = term_pos(g);
    unsigned last = term_right_stop(g);
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

void grid_set_tabs(term_grid_t *g, const uint16_t *cols)
{
    memset(g->tab_stops, 0, sizeof g->tab_stops);
    for (unsigned x = 0; x < g->width; x++)
        if (cols[x >> 4] & (1u << (x & 15)))
            g->tab_stops[x >> 3] |= (uint8_t)(1u << (x & 7));
}

/* ---- cursor movement --------------------------------------------------- */

// Up n rows. A cursor inside the region stops at its top margin; above it, at
// the top row.
static void grid_cursor_up(term_grid_t *g, uint16_t n)
{
    term_pos_t *pos = term_pos(g);
    uint8_t fence = (pos->y >= g->margin_top) ? g->margin_top : 0;
    uint16_t room = (uint16_t)(pos->y - fence);
    grid_goto(g, pos->x, (unsigned)(pos->y - (n < room ? n : room)));
}

// Down n rows. A cursor inside the region stops at its bottom margin; below
// it, at the last row.
static void grid_cursor_down(term_grid_t *g, uint16_t n)
{
    term_pos_t *pos = term_pos(g);
    uint8_t fence = (pos->y <= g->margin_bot) ? g->margin_bot
                                              : (uint8_t)(g->height - 1);
    uint16_t room = (uint16_t)(fence - pos->y);
    grid_goto(g, pos->x, (unsigned)(pos->y + (n < room ? n : room)));
}

// Right n columns: a cursor at or left of the right margin stops at it.
static void grid_cursor_forward(term_grid_t *g, uint16_t n)
{
    term_pos_t *pos = term_pos(g);
    unsigned stop = term_right_stop(g);
    unsigned room = stop - (pos->x < stop ? pos->x : stop);
    grid_goto(g, (unsigned)pos->x + (n < room ? n : room), pos->y);
}

// Left n columns: a cursor at or right of the left margin stops at it.
static void grid_cursor_back(term_grid_t *g, uint16_t n)
{
    term_pos_t *pos = term_pos(g);
    unsigned room = pos->x - term_left_stop(g);
    grid_goto(g, (unsigned)pos->x - (n < room ? n : room), pos->y);
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

// The row (or column) n past the cursor's, counted from 1 as CUP counts:
// from the margins under DECOM.
static uint16_t grid_relative(term_t *t, const term_grid_t *g, bool row, unsigned n)
{
    const term_pos_t *pos = term_pos(g);
    unsigned at = row ? pos->y : pos->x;
    unsigned from = row ? g->margin_top : g->margin_left;
    if (term_pen(t)->origin_mode)
        at = at >= from ? at - from : 0;
    unsigned v = at + 1u + n;
    return (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v);
}

// Home: the top left, or the margins' under DECOM.
static void grid_home(term_t *t, term_grid_t *g)
{
    bool origin = term_pen(t)->origin_mode;
    grid_goto(g, origin ? g->margin_left : 0, origin ? g->margin_top : 0);
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
    screen_erase(t, g->shown, pos->y, from, to, selective);
}

// Lazy-erase rows [from, to), which become single size unless the erase is
// selective: a row whose protected cells stay keeps its size, as on xterm.
static void grid_erase_rows(term_t *t, term_grid_t *g, uint8_t from, uint8_t to,
                            bool selective)
{
    if (!selective)
        screen_lines_normal(g->shown, from, to);
    screen_erase_rows(t, g->shown, from, to, selective);
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
        screen_touch(g->shown, pos->y);
        break;
    case 3:
        pos->wrap_pending = false;
        screen_forget(g->shown);
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
    grid_scroll(t, g, y, g->margin_bot, grid_param(t, 0, 1), !insert, false);
    grid_goto(g, g->margin_left, y);
}

// Scroll the region up (SU) or down (SD) by Pn rows. The cursor stays.
static void grid_SU_SD(term_t *t, term_grid_t *g, bool up)
{
    grid_scroll(t, g, g->margin_top, g->margin_bot, grid_param(t, 0, 1), up, false);
    grid_after_scroll(g);
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
// the middle are kept out of sight, and come back when the line is single
// size again, as in xterm; vttest counts losing them as a VT100 bug.
static void grid_line_size(term_grid_t *g, uint8_t size)
{
    term_pos_t *pos = term_pos(g);
    screen_set_size(g->shown, pos->y, size);
    grid_goto(g, pos->x, pos->y);
}

// DECALN, ESC # 8: the screen full of E's, for lining up a monitor. The
// margins reset, every line is single size, and the cursor goes home. The E's
// come in as a lazy erase does, a row a tick.
static void grid_DECALN(term_t *t, term_grid_t *g)
{
    grid_full_margins(g);
    screen_lines_normal(g->shown, 0, g->height);
    screen_align(t, g->shown, g->height);
    grid_goto(g, 0, 0);
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
        end = term_right_stop(g) + 1;
        n = grid_edit_count(t, g, end);
        screen_insert(t, g->shown, pos->y, pos->x, end, n);
        break;
    case 'A':
        grid_cursor_up(g, grid_param(t, 0, 1));
        break;
    case 'B':
        grid_cursor_down(g, grid_param(t, 0, 1));
        break;
    case 'e': // VPR: as VPA to the row Pn below, past the margins, as in xterm
        grid_goto(g, pos->x, grid_origin_row(t, g, grid_relative(t, g, true, grid_param(t, 0, 1))));
        break;
    case 'C':
        grid_cursor_forward(g, grid_param(t, 0, 1));
        break;
    case 'a': // HPR: as HPA to the column Pn right, past the margins, as in xterm
        grid_goto(g, grid_origin_col(t, g, grid_relative(t, g, false, grid_param(t, 0, 1))), pos->y);
        break;
    case 'D':
        grid_cursor_back(g, grid_param(t, 0, 1));
        break;
    case 'E': // CNL
        grid_cursor_down(g, grid_param(t, 0, 1));
        grid_CR(t, g);
        break;
    case 'F': // CPL
        grid_cursor_up(g, grid_param(t, 0, 1));
        grid_CR(t, g);
        break;
    case 'G':
    case '`': // HPA -- alias of CHA
        grid_goto(g, grid_origin_col(t, g, grid_param(t, 0, 1)), pos->y);
        break;
    case 'H':
    case 'f': // HVP -- alias of CUP
        grid_goto(g, grid_origin_col(t, g, grid_param(t, 1, 1)),
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
        end = term_right_stop(g) + 1;
        n = grid_edit_count(t, g, end);
        screen_delete(t, g->shown, pos->y, pos->x, end, n);
        break;
    case 'S':
        grid_SU_SD(t, g, true);
        break;
    case 'T':
        grid_SU_SD(t, g, false);
        break;
    case 'X': // ECH: Pn cells erased; the cursor doesn't move
        n = grid_edit_count(t, g, term_cols(g, pos->y));
        screen_erase(t, g->shown, pos->y, pos->x, pos->x + n, term_erase_selective(t, false));
        break;
    case 'Z': // CBT
        grid_tab_back(t, g, grid_param(t, 0, 1));
        break;
    case 'd': // VPA
        grid_goto(g, pos->x, grid_origin_row(t, g, grid_param(t, 0, 1)));
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
        grid_cursor_up(g, 1);
        break;
    case 'B': // cursor down
        grid_cursor_down(g, 1);
        break;
    case 'C': // cursor right
        grid_cursor_forward(g, 1);
        break;
    case 'D': // cursor left
        grid_cursor_back(g, 1);
        break;
    case 'H': // home
        grid_goto(g, 0, 0);
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
            screen_fill_row(t, g->shown, (uint8_t)y, (uint8_t)ch);
        else
        {
            screen_touch(g->shown, (uint8_t)y);
            screen_glyphs(t, g->shown, (uint8_t)y, r.left, (uint8_t)ch, end - r.left);
        }
    }
}

// DECERA, CSI Pt ; Pl ; Pb ; Pr $ z, erases the rectangle as ED erases the
// screen, protected cells too. DECSERA, $ {, only makes its characters
// spaces, as xterm does: the cells keep their colors and attributes, and
// after a DECSCA the protected ones are left. Neither changes a line's
// size.
static void grid_rect_erase(term_t *t, term_grid_t *g, bool wipe, bool selective)
{
    grid_rect_t r;
    if (!grid_rect_parse(t, g, 0, 4, &r))
        return;
    for (unsigned y = r.top; y <= r.bot; y++)
    {
        unsigned end = grid_rect_end(g, &r, (uint8_t)y);
        if (r.left >= end)
            continue;
        if (wipe)
        {
            screen_touch(g->shown, (uint8_t)y);
            screen_wipe(t, g->shown, (uint8_t)y, r.left, end, selective);
        }
        else if (grid_rect_whole_row(g, &r, (uint8_t)y, end))
            screen_erase_rows(t, g->shown, (uint8_t)y, (uint8_t)(y + 1), false);
        else
            grid_erase_cols(t, g, (uint8_t)y, r.left, end, false);
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
            bit = SCREEN_MARK_ALL;
            break;
        case 1:
        case 22:
            bit = SCREEN_MARK_BOLD;
            break;
        case 4:
        case 24:
            bit = SCREEN_MARK_UNDERLINE;
            break;
        case 5:
        case 25:
            bit = SCREEN_MARK_BLINK;
            break;
        case 7:
        case 27:
            bit = SCREEN_MARK_REVERSE;
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
        screen_attrs(t, g->shown, (uint8_t)y, row.left, end, set, clear, toggle);
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
        grid_rect_erase(t, g, false, false);
        break;
    case '{':
        grid_rect_erase(t, g, true, term_erase_selective(t, true));
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
            if (pos->x > term_left_stop(g))
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
            screen_shift(t, g->shown, g->margin_top, g->margin_bot, g->margin_left,
                         (unsigned)g->margin_right + 1, grid_param(t, 0, 1), ch == '@');
        break;
    case GRID_COLUMNS:
        // DECIC and DECDC, CSI Pn ' } and CSI Pn ' ~: Pn columns inserted
        // or deleted at the cursor's column, on every line of the scroll
        // region, up to the right margin. For a cursor inside the margins.
        if (grid_cursor_in_margins(g))
            screen_shift(t, g->shown, g->margin_top, g->margin_bot, pos->x,
                         (unsigned)g->margin_right + 1, grid_param(t, 0, 1), ch == '~');
        break;
    case GRID_RECT:
        grid_rect(t, g, ch);
        break;
    case GRID_LRMM:
        // DECLRMM: set, every line is single size, as on a VT420 and
        // xterm; reset, the left and right margins are the whole width.
        if (ch)
            for (int s = 0; s < 2; s++)
                screen_all_normal(&g->screen[s]);
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
                    screen_shift(t, g->shown, g->margin_top, g->margin_bot, g->margin_left,
                                 (unsigned)g->margin_right + 1, 1, !back);
            }
            else if (back)
                grid_cursor_back(g, 1);
            else
                grid_cursor_forward(g, 1);
        }
        else // RI
            grid_reverse_index(t, g);
        break;
    case GRID_LINE:
        if (ch == '8')
            grid_DECALN(t, g);
        else
            grid_line_size(g, ch == '3'   ? TERM_LINE_TOP
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
        grid_goto(g, (unsigned)(ch - 32), row);
        break;
    }
    case GRID_SAVE:
        g->shown->saved = *pos;
        break;
    case GRID_RESTORE:
    {
        // With nothing saved the slot holds home.
        *pos = g->shown->saved;
        bool pending = pos->wrap_pending;
        grid_goto(g, pos->x, pos->y);
        pos->wrap_pending = pending;
        break;
    }
    case GRID_HOME:
        grid_home(t, g);
        break;
    case GRID_STAY:
        grid_goto(g, pos->x, pos->y);
        break;
    case GRID_ALT_IN:
        g->screen[1].pos = g->screen[0].pos;
        break;
    case GRID_ALT_OUT:
        g->screen[0].pos = g->screen[1].pos;
        break;
    case GRID_CLEAR:
        screen_all_normal(g->shown);
        grid_cancel(g);
        screen_clear(t, g->shown, g->height);
        break;
    case GRID_RESET:
        grid_full_margins(g);
        grid_reset_tabs(g);
        for (int s = 0; s < 2; s++)
        {
            g->screen[s].pos.wrap_pending = false;
            memset(&g->screen[s].saved, 0, sizeof g->screen[s].saved);
        }
        screen_all_normal(&g->screen[1]);
        screen_reset(&g->screen[1], g->height);
        break;
    case GRID_DECCOLM:
        grid_full_margins(g);
        screen_all_normal(g->shown);
        grid_cancel(g);
        screen_clear(t, g->shown, g->height);
        grid_home(t, g);
        break;
    case GRID_DECSTR:
        grid_full_margins(g);
        for (int s = 0; s < 2; s++)
            memset(&g->screen[s].saved, 0, sizeof g->screen[s].saved);
        grid_goto(g, pos->x, pos->y);
        break;
    }
}

/* ---- savestate --------------------------------------------------------- */

/* Everything here indexes the cells or the line sizes. A grid with cells
 * has its width checked by their owner. */
bool term_grid_sst_ok(sst_cursor_t *c, const term_grid_t *g, uint8_t rows)
{
    const uint8_t w = g->width, h = g->height;
    if (grid_rows(g) != rows)
        return sst_refuse(c, "a grid of other rows");
    if (w < 1 || h < 1 || h > rows || g->margin_top > g->margin_bot ||
        g->margin_bot >= h || g->margin_left > g->margin_right || g->margin_right >= w)
        return sst_refuse(c, "a grid's size or margins out of range");
    const screen_t *sc = g->screen;
    for (int s = 0; s < 2; s++)
        if (sc[s].pos.x >= w || sc[s].pos.y >= h || sc[s].saved.x >= w || sc[s].saved.y >= h)
            return sst_refuse(c, "a cursor off its grid");
    return true;
}

bool term_grid_sst_lines_ok(sst_cursor_t *c, const term_grid_t *g)
{
    for (int s = 0; s < 2; s++)
    {
        const screen_t *sc = &g->screen[s];
        for (unsigned m = 0; m < sc->mem_rows; m++)
            if (sc->line[m] & ~SCREEN_LINE_SIZE)
                return sst_refuse(c, "a line attribute out of range");
        /* The cursor sits inside its own line's columns. */
        if (screen_size(sc, sc->pos.y) && sc->pos.x >= g->width / 2)
            return sst_refuse(c, "a cursor past its line's columns");
    }
    return true;
}

void term_grid_sst_put(sst_cursor_t *c, const term_grid_t *g)
{
    const uint8_t *const start = c->at;
    TERM_GRID_SST_ROWS(SST_ROW_PUT2, (*g))
    for (int s = 0; s < 2; s++)
    {
        const screen_t *sc = &g->screen[s];
        sst_put(c, sc->line, sc->mem_rows);
        sst_put_u8(c, sc->y_offset);
        sst_put_u8(c, sc->hidden);
    }
    assert(!sst_ok(c) || c->at - start == TERM_GRID_SST_SIZE(grid_rows(g)));
    (void)start;
}

/* The screens have no ring, so their rows of memory are their slots: the
 * ring's turn and the rows above row 0 index the line attributes, and the
 * rows above row 0 are no more than the height leaves. */
bool term_grid_sst_get(sst_cursor_t *c, term_grid_t *g)
{
    const uint8_t rows = grid_rows(g);
    TERM_GRID_SST_ROWS(SST_ROW_GET2, (*g))
    for (int s = 0; s < 2; s++)
    {
        screen_t *sc = &g->screen[s];
        sst_get(c, sc->line, sc->mem_rows);
        sc->y_offset = sst_get_u8(c);
        sc->hidden = sst_get_u8(c);
        sc->spare = sc->mem_rows;
    }
    if (!sst_ok(c) || !term_grid_sst_ok(c, g, rows))
        return false;
    for (int s = 0; s < 2; s++)
    {
        const screen_t *sc = &g->screen[s];
        if (sc->y_offset >= rows || sc->hidden > rows - g->height)
            return sst_refuse(c, "a screen's layout out of range");
    }
    return term_grid_sst_lines_ok(c, g);
}
