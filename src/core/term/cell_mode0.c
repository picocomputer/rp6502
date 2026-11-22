/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/cell.h"
#include "core/term/screen.h"
#include "core/vga/mode/mode.h"
#include <string.h>

// Runs of mode 0's cells (cell_mode0.h). A cell is a word, and its colors
// are three halfwords in the table beside it (mode0.h), which go wherever
// the cell goes.

// GCC turns a loop that copies or fills into a call of memmove or memset,
// and the C library's are slower here, a byte at a time.
#if defined(__GNUC__) && !defined(__clang__)
#define CELL_RUN __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define CELL_RUN
#endif

// The n cells at dst become the fill's cell, and their colors its colors.
// The cells are written eight a pass. Two cells' colors are three words, so
// the colors are written four cells a pass from the first that is on a
// word.
CELL_RUN void cell_run_fill(mode0_cell_t *dst, unsigned n, const cell_fill_t *e)
{
    const mode0_colors_t colors = e->colors;
    uint32_t w;
    memcpy(&w, &e->cell, sizeof(w));
    mode_word_t *d = (mode_word_t *)dst;
    unsigned i = n;
    for (; i >= 8; i -= 8, d += 8)
    {
        d[0] = w;
        d[1] = w;
        d[2] = w;
        d[3] = w;
        d[4] = w;
        d[5] = w;
        d[6] = w;
        d[7] = w;
    }
    for (; i; i--)
        *d++ = w;
    mode0_colors_t *c = cell_colors(dst);
    i = n;
    if (i && (cell_at(dst) & 1))
    {
        *c++ = colors;
        i--;
    }
    const mode0_colors_t two[2] = {colors, colors};
    uint32_t p[3];
    memcpy(p, two, sizeof(p));
    const uint32_t p0 = p[0], p1 = p[1], p2 = p[2];
    d = (mode_word_t *)c;
    for (; i >= 4; i -= 4, d += 6)
    {
        d[0] = p0;
        d[1] = p1;
        d[2] = p2;
        d[3] = p0;
        d[4] = p1;
        d[5] = p2;
    }
    for (; i >= 2; i -= 2, d += 3)
    {
        d[0] = p0;
        d[1] = p1;
        d[2] = p2;
    }
    if (i)
        *(mode0_colors_t *)d = colors;
}

void cell_run_fill_sel(mode0_cell_t *row, unsigned from, unsigned to, const cell_fill_t *e)
{
    mode0_colors_t *colors = cell_colors(row);
    for (unsigned i = from; i < to; i++)
        if (!cell_guarded(&row[i]))
        {
            row[i] = e->cell;
            colors[i] = e->colors;
        }
}

// The n words at src move to dst. The two may overlap, so a move toward
// lower addresses goes up from the first word and one toward higher
// addresses goes down from the last, eight words a pass. The same move as
// ldmia and stmia of eight registers, and ldmdb and stmdb for the other
// way, measured 437 and 427 cycles for 160 words against this one's 514
// and 458. It is not used, so every machine runs one path, but it is there
// to take if the cycles are wanted.
static inline __attribute__((always_inline)) void
cell_words_move(mode_word_t *d, const mode_word_t *f, unsigned w)
{
    if (d < f)
    {
        for (; w >= 8; w -= 8, d += 8, f += 8)
        {
            const uint32_t a = f[0], b = f[1], c = f[2], e = f[3];
            const uint32_t g = f[4], h = f[5], i = f[6], j = f[7];
            d[0] = a;
            d[1] = b;
            d[2] = c;
            d[3] = e;
            d[4] = g;
            d[5] = h;
            d[6] = i;
            d[7] = j;
        }
        for (; w; w--)
            *d++ = *f++;
    }
    else if (d > f)
    {
        d += w;
        f += w;
        for (; w >= 8; w -= 8)
        {
            d -= 8;
            f -= 8;
            const uint32_t a = f[7], b = f[6], c = f[5], e = f[4];
            const uint32_t g = f[3], h = f[2], i = f[1], j = f[0];
            d[7] = a;
            d[6] = b;
            d[5] = c;
            d[4] = e;
            d[3] = g;
            d[2] = h;
            d[1] = i;
            d[0] = j;
        }
        for (; w; w--)
            *--d = *--f;
    }
}

// The n cells at src, and their colors, move to dst. The colors are
// halfwords, three a cell, moved as words. Those of an odd count of cells
// are a halfword more than whole words: it goes first on the way down and
// last on the way up. A move by an odd count of cells has its words off a
// word boundary at one end, 699 cycles for a row of 80 moved a cell down
// and 610 up, against 244 for 40 cells moved by 40. Peeling a halfword so
// every read sits on a word measured no better there. A move by an even
// count from an odd cell, as from a column of one row to the same column
// of another, has both ends off a word together, and there a halfword
// peeled from each end puts every word on one.
CELL_RUN void cell_run_move(mode0_cell_t *dst, const mode0_cell_t *src, unsigned n)
{
    cell_words_move((mode_word_t *)dst, (const mode_word_t *)src, n);
    uint16_t *cd = (uint16_t *)cell_colors(dst);
    const uint16_t *cf = (const uint16_t *)cell_colors(src);
    unsigned halves = 3 * n;
    if (halves && (cell_at(dst) & cell_at(src) & 1))
    {
        if (dst < src)
        {
            *cd++ = *cf++;
            halves--;
        }
        else
        {
            // The last halfword of the run, then the first, which is off
            // a word, after the words between them.
            halves--;
            if (halves & 1)
            {
                cd[halves] = cf[halves];
                halves--;
            }
            cell_words_move((mode_word_t *)(cd + 1), (const mode_word_t *)(cf + 1), halves / 2);
            cd[0] = cf[0];
            return;
        }
    }
    if ((halves & 1) && dst > src)
        cd[halves - 1] = cf[halves - 1];
    cell_words_move((mode_word_t *)cd, (const mode_word_t *)cf, halves / 2);
    if ((halves & 1) && dst < src)
        cd[halves - 1] = cf[halves - 1];
}

void cell_wipe(mode0_cell_t *row, unsigned from, unsigned to, bool selective)
{
    for (unsigned x = from; x < to; x++)
        if (!selective || !cell_guarded(&row[x]))
        {
            row[x].glyph = ' ';
            row[x].attributes |= MODE0_ATTR_WRITTEN;
        }
}

// The bold, the reverse and the written mark are bits of the cell of their
// own, so every cell takes them alike. The underline and the blink each
// have one SCREEN_MARK_ bit for any of their styles, so what a cell comes
// to depends on whether it has one: a cell keeps, loses or gains one, as
// four masks, by whether it has an underline and a blink. An underline
// set here is a single one in the foreground's color, and a blink set here
// is the slow one. Bold, conceal and reverse are drawn from the bits
// (mode0_cell_shows), so no color changes, and no cell takes a branch.
static uint16_t cell_marks_bits(unsigned marks)
{
    return (uint16_t)(((marks & SCREEN_MARK_BOLD) ? MODE0_ATTR_BOLD : 0) |
                      ((marks & SCREEN_MARK_REVERSE) ? MODE0_ATTR_REVERSE : 0) |
                      ((marks & SCREEN_MARK_WRITTEN) ? MODE0_ATTR_WRITTEN : 0));
}

void cell_marks(mode0_cell_t *row, unsigned from, unsigned to, unsigned set, unsigned clear,
                unsigned toggle)
{
    const uint16_t bits_set = cell_marks_bits(set);
    const uint16_t bits_clear = (uint16_t)~cell_marks_bits(clear);
    const uint16_t bits_flip = cell_marks_bits(toggle);
    uint16_t keep[4], put[4];
    for (unsigned has = 0; has < 4; has++)
    {
        const unsigned marks = ((has & 1) ? SCREEN_MARK_UNDERLINE : 0) |
                               ((has & 2) ? SCREEN_MARK_BLINK : 0);
        const unsigned want = ((marks | set) & ~clear) ^ toggle;
        unsigned k = 0xFFFF, p = 0;
        if ((marks ^ want) & SCREEN_MARK_UNDERLINE)
        {
            if (want & SCREEN_MARK_UNDERLINE)
                p |= TERM_ATTR_UL(TERM_UL_SINGLE);
            else
                k &= ~(TERM_ATTR_UL_MASK | MODE0_ATTR_UL_COLOR);
        }
        if ((marks ^ want) & SCREEN_MARK_BLINK)
        {
            if (want & SCREEN_MARK_BLINK)
                p |= TERM_ATTR_BLINK;
            else
                k &= ~TERM_ATTR_ANY_BLINK;
        }
        keep[has] = (uint16_t)(k & bits_clear);
        put[has] = (uint16_t)(p | (bits_set & bits_clear));
    }
    for (mode0_cell_t *cell = row + from, *end = row + to; cell < end; cell++)
    {
        const unsigned a = cell->attributes;
        const unsigned has = (((a & TERM_ATTR_UL_MASK) + TERM_ATTR_UL_MASK) >> 5) |
                             ((((a & TERM_ATTR_ANY_BLINK) + TERM_ATTR_ANY_BLINK) >> 2) << 1);
        cell->attributes = (uint16_t)(((a & keep[has]) | put[has]) ^ bits_flip);
    }
}

CELL_RUN void cell_strip_keep(cell_strip_t *strip, const mode0_cell_t *row, unsigned from,
                              unsigned end, unsigned width)
{
    const mode0_colors_t *colors = cell_colors(row);
    unsigned k = 0;
    for (unsigned i = 0; i < from; i++, k++)
    {
        strip->cell[k] = row[i];
        strip->colors[k] = colors[i];
    }
    for (unsigned i = end; i < width; i++, k++)
    {
        strip->cell[k] = row[i];
        strip->colors[k] = colors[i];
    }
}

CELL_RUN void cell_strip_put(const cell_strip_t *strip, mode0_cell_t *row, unsigned from,
                             unsigned end, unsigned width)
{
    mode0_colors_t *colors = cell_colors(row);
    unsigned k = 0;
    for (unsigned i = 0; i < from; i++, k++)
    {
        row[i] = strip->cell[k];
        colors[i] = strip->colors[k];
    }
    for (unsigned i = end; i < width; i++, k++)
    {
        row[i] = strip->cell[k];
        colors[i] = strip->colors[k];
    }
}

CELL_RUN void cell_strip_move(mode0_cell_t *to, const mode0_cell_t *row, unsigned from,
                              unsigned end, unsigned width)
{
    mode0_colors_t *tc = cell_colors(to);
    const mode0_colors_t *rc = cell_colors(row);
    if (from > 4)
        cell_run_move(to, row, from);
    else
        for (unsigned i = 0; i < from; i++)
        {
            to[i] = row[i];
            tc[i] = rc[i];
        }
    if (width - end > 4)
        cell_run_move(to + end, row + end, width - end);
    else
        for (unsigned i = end; i < width; i++)
        {
            to[i] = row[i];
            tc[i] = rc[i];
        }
}
