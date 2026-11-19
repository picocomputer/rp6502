/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/bel.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/term/color.h"
#include "ria/sys/vga.h"
#include "ria/term/cells.h"
#include "machine.h"

/* The console's terminal on the RIA, at the VGA's two sizes. Each grid keeps
 * only its line sizes, a byte a row for each screen. */
term_t cells_term;
static term_grid_t cells_40, cells_80;
static uint8_t cells_40_lines[2 * TERM_MAX_HEIGHT];
static uint8_t cells_80_lines[2 * TERM_MAX_HEIGHT];

static void cells_out_chars(const char *buf, int length)
{
    for (int i = 0; i < length; i++)
        term_write(&cells_term, (uint8_t)buf[i]);
}

void cells_init(void)
{
    term_grid_init(&cells_40, 40, cells_40_lines, TERM_MAX_HEIGHT);
    term_grid_init(&cells_80, 80, cells_80_lines, TERM_MAX_HEIGHT);
    term_grid_t *const grids[] = {&cells_40, &cells_80};
    term_init(&cells_term, grids, 2);
    manifold_set_term_out(cells_out_chars);
}

void cells_flashing(void)
{
    manifold_set_term_out(NULL);
}

void cells_RIS(void)
{
    term_RIS(&cells_term);
}

void cells_RIS_no_clear(void)
{
    term_set_height(&cells_term, &cells_40, TERM_STD_HEIGHT);
    term_RIS_no_clear(&cells_term);
}

void cells_set_height(uint8_t width, uint8_t height)
{
    term_set_height(&cells_term, width == 40 ? &cells_40 : &cells_80, height);
}

void cells_reply(const term_t *t, const char *s, size_t n)
{
    (void)t;
    com_reply(s, n);
}

void cells_bell(const term_t *t)
{
    (void)t;
    if (com_get_bel())
        bel_add(&bel_teletype);
}

/* The 40 on a 320-wide canvas of a connected VGA; otherwise the 80, which is
 * the console with no VGA too. */
term_grid_t *cells_shown(const term_t *t)
{
    (void)t;
    int w, h;
    vga_canvas_size(&w, &h);
    return vga_connected() && w == 320 ? &cells_40 : &cells_80;
}

/* Nothing is drawn, and no cell is kept. What is kept is whether each row
 * of each screen may have something on it, a bit a row, for cells_row_used:
 * a character that is not a space, a background that is not the default
 * one, or a line through a cell. A bit is set by what puts any of those on
 * a row and cleared only by what erases the whole of one, so it says more
 * rows are in use than are, and never fewer. */
static uint32_t cells_used[2][2]; /* the 40 and the 80, each screen of it */
_Static_assert(TERM_MAX_HEIGHT <= 32, "a bit for every row");

/* The rows of a grid's screen showing, or NULL for a grid that is not the
 * VGA's: a program's own size. */
static uint32_t *cells_rows(const term_t *t, const term_grid_t *g)
{
    if (g != &cells_40 && g != &cells_80)
        return NULL;
    return &cells_used[g == &cells_80][t->alt_active ? 1 : 0];
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
static void cells_rows_erased(term_t *t, term_grid_t *g, unsigned from, unsigned to, bool whole)
{
    uint32_t *rows = cells_rows(t, g);
    if (!rows)
        return;
    if (cells_erase_shows(t))
        *rows |= cells_bits(from, to);
    else if (whole)
        *rows &= ~cells_bits(from, to);
}

bool cells_row_used(term_t *t, term_grid_t *g, uint8_t y)
{
    const uint32_t *rows = cells_rows(t, g);
    return rows && (*rows >> y & 1);
}

void cells_touch(term_t *t, term_grid_t *g, uint8_t y)
{
    (void)t, (void)g, (void)y;
}

void cells_glyphs(term_t *t, term_grid_t *g, uint8_t y, uint8_t x, uint8_t byte, unsigned n)
{
    (void)x, (void)n;
    uint32_t *rows = cells_rows(t, g);
    if (rows && (byte != ' ' || cells_space_shows(t)))
        *rows |= 1u << y;
}

void cells_erase(term_t *t, term_grid_t *g, uint8_t y, unsigned from, unsigned to,
                 bool selective)
{
    cells_rows_erased(t, g, y, y + 1u, !selective && !from && to >= term_cols(g, y));
}

void cells_insert(term_t *t, term_grid_t *g, uint8_t y, uint8_t x, unsigned end, unsigned n)
{
    (void)x, (void)end, (void)n;
    cells_rows_erased(t, g, y, y + 1u, false);
}

void cells_delete(term_t *t, term_grid_t *g, uint8_t y, uint8_t x, unsigned end, unsigned n)
{
    (void)x, (void)end, (void)n;
    cells_rows_erased(t, g, y, y + 1u, false);
}

void cells_shift(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, unsigned from,
                 unsigned end, unsigned n, bool left)
{
    (void)from, (void)end, (void)n, (void)left;
    cells_rows_erased(t, g, top, bot + 1u, false);
}

/* Rows [top, bot] move n rows, and n rows come in. With whole the rows
 * that come in are erased. Without it only some of their columns are, and
 * a row that moves lands on what the other columns of its new row hold. */
static void cells_rows_scroll(term_t *t, term_grid_t *g, unsigned top, unsigned bot,
                              unsigned n, bool up, bool whole)
{
    uint32_t *rows = cells_rows(t, g);
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
    cells_rows_erased(t, g, up ? bot + 1 - n : top, up ? bot + 1 : top + n, whole);
}

void cells_scroll_cols(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, unsigned from,
                       unsigned end, uint8_t n, bool up)
{
    (void)from, (void)end;
    cells_rows_scroll(t, g, top, bot, n, up, false);
}

void cells_copy(term_t *t, term_grid_t *g, uint8_t sy, unsigned sx, uint8_t dy, unsigned dx,
                unsigned n)
{
    (void)sx, (void)dx, (void)n;
    uint32_t *rows = cells_rows(t, g);
    if (rows && (*rows >> sy & 1))
        *rows |= 1u << dy;
}

/* Reverse and an underline show on a cell with nothing else on it. */
void cells_attrs(term_t *t, term_grid_t *g, uint8_t y, unsigned from, unsigned to,
                 unsigned set, unsigned clear, unsigned toggle)
{
    (void)from, (void)to, (void)clear;
    uint32_t *rows = cells_rows(t, g);
    if (rows && ((set | toggle) & (CELLS_MARK_REVERSE | CELLS_MARK_UNDERLINE)))
        *rows |= 1u << y;
}

void cells_erase_rows(term_t *t, term_grid_t *g, uint8_t from, uint8_t to,
                      bool selective)
{
    cells_rows_erased(t, g, from, to, !selective);
}

void cells_align(term_t *t, term_grid_t *g)
{
    uint32_t *rows = cells_rows(t, g);
    if (rows)
        *rows = cells_bits(0, g->height);
}

void cells_fill_row(term_t *t, term_grid_t *g, uint8_t y, uint8_t byte)
{
    uint32_t *rows = cells_rows(t, g);
    if (!rows)
        return;
    if (byte != ' ' || cells_space_shows(t))
        *rows |= 1u << y;
    else
        *rows &= ~(1u << y);
}

void cells_scroll(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, uint8_t n, bool up)
{
    cells_rows_scroll(t, g, top, bot, n, up, true);
}

void cells_index(term_t *t, term_grid_t *g, bool up)
{
    cells_rows_scroll(t, g, 0, g->height - 1u, 1, up, true);
}

void cells_forget(term_t *t, term_grid_t *g)
{
    (void)t, (void)g;
}

void cells_clear(term_t *t, term_grid_t *g)
{
    cells_rows_erased(t, g, 0, 32, true);
}

/* A row that comes back from the scrollback has what it had, which is not
 * known here. One that goes there, or a new one at the bottom, leaves the
 * rest as they were. */
void cells_height(term_grid_t *g, bool grow, bool top)
{
    if (g != &cells_40 && g != &cells_80)
        return;
    for (int screen = 0; screen < 2; screen++)
    {
        uint32_t *rows = &cells_used[g == &cells_80][screen];
        if (top)
            *rows = grow ? *rows << 1 | 1 : *rows >> 1;
        *rows &= cells_bits(0, g->height);
    }
}

/* The alternate screen is erased. */
void cells_reset(term_t *t, term_grid_t *g)
{
    (void)t;
    if (g == &cells_40 || g == &cells_80)
        cells_used[g == &cells_80][1] = 0;
}

/* The colors are followed, for a program that asks what one is: the
 * palette, then the default foreground, the default background and the
 * cursor's. */
#define CELLS_FG_COLOR_INDEX 7
#define CELLS_BG_COLOR_INDEX 0
static uint16_t cells_colors[256 + 3];

void cells_color(term_t *t, unsigned which, uint16_t color)
{
    (void)t;
    if (which < sizeof cells_colors / sizeof cells_colors[0])
        cells_colors[which] = color;
}

void cells_color_reset(term_t *t, unsigned which)
{
    cells_color(t, which, color_256[which < 256                 ? which
                                    : which == CELLS_COLOR_BG ? CELLS_BG_COLOR_INDEX
                                                              : CELLS_FG_COLOR_INDEX]);
}

void cells_colors_reset(term_t *t)
{
    for (unsigned i = 0; i < sizeof cells_colors / sizeof cells_colors[0]; i++)
        cells_color_reset(t, i);
}

uint16_t cells_color_get(const term_t *t, unsigned which)
{
    (void)t;
    return which < sizeof cells_colors / sizeof cells_colors[0] ? cells_colors[which] : 0;
}
