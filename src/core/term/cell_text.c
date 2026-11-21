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
void cell_marks(cell_t *row, unsigned from, unsigned to, unsigned set, unsigned clear,
                unsigned toggle)
{
    for (unsigned i = from; i < to; i++)
    {
        const unsigned cell = row[i];
        const unsigned has = ((cell & CELL_TEXT_UNDERLINE) ? SCREEN_MARK_UNDERLINE : 0) |
                             ((cell & CELL_TEXT_REVERSE) ? SCREEN_MARK_REVERSE : 0) |
                             ((cell & CELL_TEXT_WRITTEN) ? SCREEN_MARK_WRITTEN : 0);
        const unsigned want = ((has | set) & ~clear) ^ toggle;
        unsigned next = cell & ~(CELL_TEXT_UNDERLINE | CELL_TEXT_REVERSE | CELL_TEXT_WRITTEN);
        if (want & SCREEN_MARK_UNDERLINE)
            next |= CELL_TEXT_UNDERLINE;
        if (want & SCREEN_MARK_REVERSE)
            next |= CELL_TEXT_REVERSE;
        if (want & SCREEN_MARK_WRITTEN)
            next |= CELL_TEXT_WRITTEN;
        row[i] = (cell_t)next;
    }
}

void cell_strip_keep(cell_strip_t *strip, const cell_t *row, unsigned from, unsigned end,
                     unsigned width)
{
    memcpy(strip->cell, row, from * sizeof(cell_t));
    memcpy(strip->cell + from, row + end, (width - end) * sizeof(cell_t));
}

void cell_strip_put(const cell_strip_t *strip, cell_t *row, unsigned from, unsigned end,
                    unsigned width)
{
    memcpy(row, strip->cell, from * sizeof(cell_t));
    memcpy(row + end, strip->cell + from, (width - end) * sizeof(cell_t));
}

void cell_strip_move(cell_t *to, const cell_t *row, unsigned from, unsigned end,
                     unsigned width)
{
    memcpy(to, row, from * sizeof(cell_t));
    memcpy(to + end, row + end, (width - end) * sizeof(cell_t));
}
