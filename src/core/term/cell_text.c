/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/cell.h"
#include "core/term/screen.h"
#include <string.h>

// Runs of text cells (cell_text.h). A cell is a halfword and nothing goes
// with it.

void cell_run_fill(cell_t *dst, unsigned n, const cell_fill_t *e)
{
    const cell_t cell = e->cell;
    for (; n; n--)
        *dst++ = cell;
}

void cell_run_fill_sel(cell_t *row, unsigned from, unsigned to, const cell_fill_t *e)
{
    for (unsigned i = from; i < to; i++)
        if (!cell_guarded(&row[i]))
            row[i] = e->cell;
}

void cell_run_move(cell_t *dst, const cell_t *src, unsigned n)
{
    memmove(dst, src, n * sizeof(cell_t));
}

void cell_wipe(cell_t *row, unsigned from, unsigned to, bool selective)
{
    for (unsigned x = from; x < to; x++)
        if (!selective || !cell_guarded(&row[x]))
            row[x] = (cell_t)((row[x] & ~(CELL_TEXT_CODE | CELL_TEXT_SET)) | ' ' |
                              CELL_TEXT_WRITTEN);
}

// Only the underline and the swap show on a text cell: bold and the blinks
// change its colors and its drawing, which it has none of. An underline
// that is set or cleared leaves the strike and the overline, and a swap
// leaves the paper (cell_text.h). Whether it was written is the cell's too.
// Each is one bit of the cell, so every cell takes the same three masks.
static cell_t cell_marks_bits(unsigned marks)
{
    return (cell_t)(((marks & SCREEN_MARK_UNDERLINE) ? CELL_TEXT_UNDERLINE : 0) |
                    ((marks & SCREEN_MARK_REVERSE) ? CELL_TEXT_REVERSE : 0) |
                    ((marks & SCREEN_MARK_WRITTEN) ? CELL_TEXT_WRITTEN : 0));
}

void cell_marks(cell_t *row, unsigned from, unsigned to, unsigned set, unsigned clear,
                unsigned toggle)
{
    const unsigned keep = (cell_t)~cell_marks_bits(clear);
    const unsigned put = cell_marks_bits(set) & keep;
    const unsigned flip = cell_marks_bits(toggle);
    for (cell_t *cell = row + from, *end = row + to; cell < end; cell++)
        *cell = (cell_t)(((*cell & keep) | put) ^ flip);
}
