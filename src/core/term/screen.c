/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/screen.h"
#include "core/term/term.h"
#include <string.h>

// The ring a screen's rows are laid out in (screen.h). Every machine keeps
// it for every screen, with cells or without.

void screen_lines_normal(screen_t *s, unsigned from, unsigned to)
{
    for (unsigned y = from; y < to; y++)
        s->line[screen_mem(s, (uint8_t)y)] = TERM_LINE_NORMAL;
}

void screen_all_normal(screen_t *s)
{
    memset(s->line, TERM_LINE_NORMAL, s->mem_rows);
}

void screen_init(screen_t *s, uint8_t rows, uint8_t *line, uint8_t *row_idx)
{
    s->rows = rows;
    s->mem_rows = (uint8_t)(row_idx ? rows + TERM_SPARE_ROWS : rows);
    s->line = line;
    s->row_idx = row_idx;
    s->cells = NULL;
    screen_all_normal(s);
    screen_clear(s);
}

void screen_set_cells(screen_t *s, struct screen_cells *cells)
{
    s->cells = cells;
}

void screen_clear(screen_t *s)
{
    s->y_offset = 0;
    s->hidden = 0;
    s->spare = s->rows;
    if (s->row_idx)
        for (unsigned y = 0; y < s->rows; y++)
            s->row_idx[y] = (uint8_t)y;
}

// Rows [lo, hi] of the screen swap their rows of memory end for end. With
// no ring, a slot's row of memory is the slot's own, so the rows' line
// attributes swap instead.
static void screen_reverse(screen_t *s, unsigned lo, unsigned hi)
{
    uint8_t *const ring = s->row_idx ? s->row_idx : s->line;
    for (; lo < hi; lo++, hi--)
    {
        uint8_t *a = &ring[screen_slot(s, (uint8_t)lo)];
        uint8_t *b = &ring[screen_slot(s, (uint8_t)hi)];
        const uint8_t m = *a;
        *a = *b;
        *b = m;
    }
}

// The rows turn k places toward the top: the first k reversed, the rest
// reversed, then all of them reversed. No row needs keeping aside, so a
// ring of any size turns in place.
void screen_rotate(screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up)
{
    const unsigned h = (unsigned)bot - top + 1;
    const unsigned k = up ? n : h - n;
    if (!k || k >= h)
        return;
    screen_reverse(s, top, top + k - 1);
    screen_reverse(s, top + k, bot);
    screen_reverse(s, top, bot);
}

// The ring turns a slot: up, row 0 is the row that was row 1; down, the row
// that was the last slot's.
static void screen_turn(screen_t *s, bool up)
{
    if (up)
        s->y_offset = (uint8_t)(s->y_offset + 1u == s->rows ? 0 : s->y_offset + 1u);
    else
        s->y_offset = (uint8_t)(s->y_offset ? s->y_offset - 1u : s->rows - 1u);
}

// The row of memory of row gone, which just went out, is the one a smooth
// scroll draws going. With keep, when row in, which comes in, is in the
// same slot, the spare row of memory takes that slot.
static uint8_t screen_keep(screen_t *s, uint8_t in, uint8_t gone, bool keep)
{
    const uint8_t slot = screen_slot(s, gone);
    const uint8_t ghost = s->row_idx ? s->row_idx[slot] : slot;
    if (keep && in == gone && s->spare != s->mem_rows)
    {
        s->row_idx[slot] = s->spare;
        s->spare = ghost;
    }
    return ghost;
}

uint8_t screen_scroll(screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up, bool keep)
{
    screen_rotate(s, top, bot, n, up);
    const uint8_t edge = (uint8_t)(up ? bot - n + 1 : top + n - 1);
    const uint8_t ghost = screen_keep(s, edge, edge, keep);
    if (up)
        screen_lines_normal(s, edge, (unsigned)bot + 1);
    else
        screen_lines_normal(s, top, (unsigned)edge + 1);
    return ghost;
}

uint8_t screen_index(screen_t *s, uint8_t height, bool up, bool keep)
{
    screen_turn(s, up);
    const uint8_t in = (uint8_t)(up ? height - 1 : 0);
    uint8_t ghost;
    if (up)
    {
        if (s->hidden < s->rows - height)
            s->hidden++;
        ghost = screen_keep(s, in, (uint8_t)(s->rows - 1), keep);
    }
    else
    {
        if (s->hidden)
            s->hidden--;
        ghost = screen_keep(s, in, (uint8_t)(height == s->rows ? 0 : height), keep);
    }
    screen_lines_normal(s, in, in + 1u);
    return ghost;
}

bool screen_height(screen_t *s, uint8_t height, bool grow, bool top)
{
    if (!top)
    {
        if (grow)
            screen_lines_normal(s, height - 1u, height);
        return false;
    }
    screen_turn(s, !grow);
    if (!grow)
        s->hidden++;
    else if (s->hidden)
        s->hidden--;
    else
    {
        screen_lines_normal(s, 0, 1);
        return true;
    }
    return false;
}

void screen_fit(screen_t *s, uint8_t height)
{
    const uint8_t room = (uint8_t)(s->rows - height);
    if (s->hidden > room)
        s->hidden = room;
}
