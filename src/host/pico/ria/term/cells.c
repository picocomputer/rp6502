/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/manifold.h"
#include "core/term/console_term.h"
#include "core/term/grid.h"
#include "ria/sys/vga.h"
#include "ria/term/cells.h"
#include "machine.h"

/* The console's terminal on the RIA (console_term.h), handed no cells. */
static int cells_out_chars(const char *buf, int length, bool force)
{
    (void)force;
    for (int i = 0; i < length; i++)
        term_write(&console_term, (uint8_t)buf[i]);
    return length;
}

void cells_init(void)
{
    console_init(NULL);
    manifold_set_term_out(cells_out_chars);
}

void cells_flashing(void)
{
    manifold_set_term_out(NULL);
}

/* Nothing is drawn, so there are no blinks to start again. */
void draw_reset(void)
{
}

/* Nothing is drawn, and no cell is kept (TERM_CELLS 0): this file defines
 * the cell store's functions that change cells (screen.h), and they move a
 * screen's ring and keep whether each row of each screen may have
 * something on it, a bit a row, for screen_row_used: a character that is
 * not a space, a background that is not the default one, or a line through
 * a cell. A bit is set by what puts any of those on a row and cleared only
 * by what erases the whole of one, so it says more rows are in use than
 * are, and never fewer. */
static uint32_t cells_used[2][2]; /* the 40 and the 80, each screen of it */
_Static_assert(TERM_MAX_HEIGHT <= 32, "a bit for every row");

/* The grid a screen is of, or NULL for a screen of a grid that is not the
 * VGA's: a program's own size. */
static term_grid_t *cells_grid(const screen_t *s)
{
    if (s == &console_40.grid.screen[0] || s == &console_40.grid.screen[1])
        return &console_40.grid;
    if (s == &console_80.grid.screen[0] || s == &console_80.grid.screen[1])
        return &console_80.grid;
    return NULL;
}

/* The rows of a screen, or NULL for a screen of a program's own size. */
static uint32_t *cells_rows(const screen_t *s)
{
    const term_grid_t *g = cells_grid(s);
    if (!g)
        return NULL;
    return &cells_used[g == &console_80.grid][s - g->screen];
}

static uint32_t cells_bits(unsigned from, unsigned to) /* rows [from, to) */
{
    uint32_t bits = 0;
    for (unsigned y = from; y < to && y < 32; y++)
        bits |= 1u << y;
    return bits;
}

/* An erased cell has the pen's background. A written one has all of the
 * pen: a space is something to see when the pen is reversed or has a line. */
static bool cells_erase_shows(const term_t *t)
{
    return term_pen(t)->bg.kind != TERM_COLOR_DEFAULT;
}

static bool cells_space_shows(const term_t *t)
{
    const term_pen_t *pen = term_pen(t);
    return cells_erase_shows(t) || pen->reverse ||
           (pen->sgr_attr & (TERM_ATTR_UL_MASK | TERM_ATTR_STRIKE | TERM_ATTR_OVERLINE));
}

/* Rows [from, to) are erased whole, or have erased cells come into them. */
static void cells_rows_erased(term_t *t, screen_t *s, unsigned from, unsigned to, bool whole)
{
    uint32_t *rows = cells_rows(s);
    if (!rows)
        return;
    if (cells_erase_shows(t))
        *rows |= cells_bits(from, to);
    else if (whole)
        *rows &= ~cells_bits(from, to);
}

bool screen_row_used(screen_t *s, uint8_t y)
{
    const uint32_t *rows = cells_rows(s);
    return rows && (*rows >> y & 1);
}

void screen_touch(screen_t *s, uint8_t y)
{
    (void)s, (void)y;
}

void screen_glyphs(term_t *t, screen_t *s, uint8_t y, uint8_t x, uint8_t byte, unsigned n)
{
    (void)x, (void)n;
    uint32_t *rows = cells_rows(s);
    if (rows && (byte != ' ' || cells_space_shows(t)))
        *rows |= 1u << y;
}

void screen_erase(term_t *t, screen_t *s, uint8_t y, unsigned from, unsigned to,
                  bool selective)
{
    const term_grid_t *g = cells_grid(s);
    if (!g)
        return;
    const unsigned cols = screen_size(s, y) ? g->width / 2u : g->width;
    cells_rows_erased(t, s, y, y + 1u, !selective && !from && to >= cols);
}

/* The cells keep their colors, so a row is as used as it was. */
void screen_wipe(term_t *t, screen_t *s, uint8_t y, unsigned from, unsigned to,
                 bool selective)
{
    (void)t, (void)s, (void)y, (void)from, (void)to, (void)selective;
}

void screen_insert(term_t *t, screen_t *s, uint8_t y, uint8_t x, unsigned end, unsigned n)
{
    (void)x, (void)end, (void)n;
    cells_rows_erased(t, s, y, y + 1u, false);
}

void screen_delete(term_t *t, screen_t *s, uint8_t y, uint8_t x, unsigned end, unsigned n)
{
    (void)x, (void)end, (void)n;
    cells_rows_erased(t, s, y, y + 1u, false);
}

void screen_shift(term_t *t, screen_t *s, uint8_t top, uint8_t bot, unsigned from,
                  unsigned end, unsigned n, bool left)
{
    (void)from, (void)end, (void)n, (void)left;
    cells_rows_erased(t, s, top, bot + 1u, false);
}

/* Rows [top, bot] move n rows, and n rows come in. With whole the rows
 * that come in are erased. Without it only some of their columns are, and
 * a row that moves lands on what the other columns of its new row hold. */
static void cells_rows_scroll(term_t *t, screen_t *s, unsigned top, unsigned bot,
                              unsigned n, bool up, bool whole)
{
    uint32_t *rows = cells_rows(s);
    if (!rows || top > bot)
        return;
    const unsigned count = bot - top + 1;
    if (n > count)
        n = count;
    const uint32_t region = cells_bits(top, bot + 1);
    const uint32_t moved = (up ? *rows >> n : *rows << n) & region;
    const uint32_t in = up ? cells_bits(bot + 1 - n, bot + 1) : cells_bits(top, top + n);
    if (whole)
        *rows = (*rows & ~region) | (moved & ~in);
    else
        *rows |= moved & ~in;
    cells_rows_erased(t, s, up ? bot + 1 - n : top, up ? bot + 1 : top + n, whole);
}

void screen_scroll_cols(term_t *t, screen_t *s, uint8_t top, uint8_t bot, unsigned from,
                        unsigned end, uint8_t n, bool up)
{
    (void)from, (void)end;
    cells_rows_scroll(t, s, top, bot, n, up, false);
}

void screen_copy_cells(term_t *t, screen_t *s, uint8_t sy, unsigned sx, uint8_t dy,
                       unsigned dx, unsigned n)
{
    (void)t, (void)sx, (void)dx, (void)n;
    uint32_t *rows = cells_rows(s);
    if (rows && (*rows >> sy & 1))
        *rows |= 1u << dy;
}

/* Reverse and an underline show on a cell with nothing else on it. */
void screen_attrs(term_t *t, screen_t *s, uint8_t y, unsigned from, unsigned to,
                  unsigned set, unsigned clear, unsigned toggle)
{
    (void)t, (void)from, (void)to, (void)clear;
    uint32_t *rows = cells_rows(s);
    if (rows && ((set | toggle) & (SCREEN_MARK_REVERSE | SCREEN_MARK_UNDERLINE)))
        *rows |= 1u << y;
}

void screen_erase_rows(term_t *t, screen_t *s, uint8_t from, uint8_t to, bool selective)
{
    cells_rows_erased(t, s, from, to, !selective);
}

void screen_align(term_t *t, screen_t *s, uint8_t height)
{
    (void)t;
    uint32_t *rows = cells_rows(s);
    if (rows)
        *rows = cells_bits(0, height);
}

void screen_fill_row(term_t *t, screen_t *s, uint8_t y, uint8_t byte)
{
    uint32_t *rows = cells_rows(s);
    if (!rows)
        return;
    if (byte != ' ' || cells_space_shows(t))
        *rows |= 1u << y;
    else
        *rows &= ~(1u << y);
}

uint8_t screen_scroll(term_t *t, screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up,
                      bool keep)
{
    const uint8_t ghost = screen_ring_scroll(s, top, bot, n, up, keep);
    cells_rows_scroll(t, s, top, bot, n, up, true);
    return ghost;
}

uint8_t screen_index(term_t *t, screen_t *s, uint8_t height, bool up, bool keep)
{
    const uint8_t ghost = screen_ring_index(s, height, up, keep);
    cells_rows_scroll(t, s, 0, height - 1u, 1, up, true);
    return ghost;
}

void screen_clear(term_t *t, screen_t *s, uint8_t height)
{
    (void)height;
    screen_ring_clear(s);
    cells_rows_erased(t, s, 0, 32, true);
}

/* A row that comes back from the scrollback has what it had, which is not
 * known here. One that goes there, or a new one at the bottom, leaves the
 * rest as they were. The rows of both screens of the size move. */
void screen_height(screen_t *s, uint8_t height, bool grow, bool top)
{
    screen_ring_height(s, height, grow, top);
    term_grid_t *g = cells_grid(s);
    if (!g)
        return;
    for (int screen = 0; screen < 2; screen++)
    {
        uint32_t *rows = &cells_used[g == &console_80.grid][screen];
        if (top)
            *rows = grow ? *rows << 1 | 1 : *rows >> 1;
        *rows &= cells_bits(0, height);
    }
}

void screen_fit(screen_t *s, uint8_t height)
{
    screen_ring_fit(s, height);
}

/* The alternate screen is erased. */
void screen_reset(screen_t *s, uint8_t height)
{
    (void)height;
    screen_ring_clear(s);
    uint32_t *rows = cells_rows(s);
    if (rows)
        *rows = 0;
}
