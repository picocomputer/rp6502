/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

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

/* The 40 on a 320-wide canvas of a connected VGA; otherwise the 80, which is
 * the console with no VGA too. */
term_grid_t *cells_shown(const term_t *t)
{
    (void)t;
    int w, h;
    vga_canvas_size(&w, &h);
    return vga_connected() && w == 320 ? &cells_40 : &cells_80;
}

/* Nothing is drawn. */

void cells_touch(term_t *t, term_grid_t *g, uint8_t y)
{
    (void)t, (void)g, (void)y;
}

void cells_glyphs(term_t *t, term_grid_t *g, uint8_t y, uint8_t x, uint8_t byte, unsigned n)
{
    (void)t, (void)g, (void)y, (void)x, (void)byte, (void)n;
}

void cells_erase(term_t *t, term_grid_t *g, uint8_t y, unsigned from, unsigned to,
                 bool selective)
{
    (void)t, (void)g, (void)y, (void)from, (void)to, (void)selective;
}

void cells_insert(term_t *t, term_grid_t *g, uint8_t y, uint8_t x, unsigned end, unsigned n)
{
    (void)t, (void)g, (void)y, (void)x, (void)end, (void)n;
}

void cells_delete(term_t *t, term_grid_t *g, uint8_t y, uint8_t x, unsigned end, unsigned n)
{
    (void)t, (void)g, (void)y, (void)x, (void)end, (void)n;
}

void cells_shift(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, unsigned from,
                 unsigned end, unsigned n, bool left)
{
    (void)t, (void)g, (void)top, (void)bot, (void)from, (void)end, (void)n, (void)left;
}

void cells_scroll_cols(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, unsigned from,
                       unsigned end, uint8_t n, bool up)
{
    (void)t, (void)g, (void)top, (void)bot, (void)from, (void)end, (void)n, (void)up;
}

void cells_copy(term_t *t, term_grid_t *g, uint8_t sy, unsigned sx, uint8_t dy, unsigned dx,
                unsigned n)
{
    (void)t, (void)g, (void)sy, (void)sx, (void)dy, (void)dx, (void)n;
}

void cells_attrs(term_t *t, term_grid_t *g, uint8_t y, unsigned from, unsigned to,
                 unsigned set, unsigned clear, unsigned toggle)
{
    (void)t, (void)g, (void)y, (void)from, (void)to, (void)set, (void)clear, (void)toggle;
}

void cells_erase_rows(term_t *t, term_grid_t *g, uint8_t from, uint8_t to,
                      bool selective)
{
    (void)t, (void)g, (void)from, (void)to, (void)selective;
}

void cells_align(term_t *t, term_grid_t *g)
{
    (void)t, (void)g;
}

void cells_fill_row(term_t *t, term_grid_t *g, uint8_t y, uint8_t byte)
{
    (void)t, (void)g, (void)y, (void)byte;
}

void cells_scroll(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, uint8_t n, bool up)
{
    (void)t, (void)g, (void)top, (void)bot, (void)n, (void)up;
}

void cells_index(term_t *t, term_grid_t *g, bool up)
{
    (void)t, (void)g, (void)up;
}

void cells_forget(term_t *t, term_grid_t *g)
{
    (void)t, (void)g;
}

void cells_clear(term_t *t, term_grid_t *g)
{
    (void)t, (void)g;
}

void cells_height(term_grid_t *g, bool grow, bool top)
{
    (void)g, (void)grow, (void)top;
}

void cells_reset(term_t *t, term_grid_t *g)
{
    (void)t, (void)g;
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
