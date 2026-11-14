/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/manifold.h"
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
    manifold_reply(s, n);
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

void cells_erase(term_t *t, term_grid_t *g, uint8_t y, unsigned from, unsigned to)
{
    (void)t, (void)g, (void)y, (void)from, (void)to;
}

void cells_insert(term_t *t, term_grid_t *g, uint8_t y, uint8_t x, unsigned n)
{
    (void)t, (void)g, (void)y, (void)x, (void)n;
}

void cells_delete(term_t *t, term_grid_t *g, uint8_t y, uint8_t x, unsigned n)
{
    (void)t, (void)g, (void)y, (void)x, (void)n;
}

void cells_shift(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, unsigned n, bool left)
{
    (void)t, (void)g, (void)top, (void)bot, (void)n, (void)left;
}

void cells_erase_rows(term_t *t, term_grid_t *g, uint8_t from, uint8_t to)
{
    (void)t, (void)g, (void)from, (void)to;
}

void cells_align(term_t *t, term_grid_t *g)
{
    (void)t, (void)g;
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

void cells_height(term_t *t, term_grid_t *g, bool grow, bool top)
{
    (void)t, (void)g, (void)grow, (void)top;
}

void cells_reset(term_t *t, term_grid_t *g)
{
    (void)t, (void)g;
}

void cells_colors_reset(term_t *t)
{
    (void)t;
}

void cells_color(term_t *t, unsigned which, uint16_t color)
{
    (void)t, (void)which, (void)color;
}

void cells_color_reset(term_t *t, unsigned which)
{
    (void)t, (void)which;
}
