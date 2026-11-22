/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_CELL_TEXT_H_
#define _CORE_TERM_CELL_TEXT_H_

/* The text cell (cell.h), which includes this: a cell's text, and whether
 * it shows, in a halfword, for a machine that keeps the console's text but
 * draws nothing. It has no colors and no font, so a cell is the same at
 * either width.
 *
 * Its low bits are its text as cell_text gives it (cell.h): the character,
 * its set, whether it was written, and its guard. The rest say whether it
 * shows, as mode 0's cell_shows has it of the same cell: a line through
 * it, the underline apart from the strike and the overline, because
 * DECCARA and DECRARA change the one and not the others; its colors
 * swapped; and its paper, which is whether its background is other than
 * the default with its colors not swapped. A swap of the colors leaves the
 * paper as it is, so a cell that DECRARA swaps back shows its paper again,
 * as mode 0's does. Bold, faint and the blinks are colors and drawing, so
 * the cell holds none of them. */

#include "core/sys/sst.h"
#include "core/term/term.h"
#include <stdbool.h>
#include <stdint.h>

typedef uint16_t cell_t;

#define CELL_TEXT_UNDERLINE (1u << 10)
#define CELL_TEXT_LINES (1u << 11)
#define CELL_TEXT_REVERSE (1u << 12)
#define CELL_TEXT_PAPER (1u << 15)
#define CELL_TEXT_SHOWS (CELL_TEXT_UNDERLINE | CELL_TEXT_LINES | CELL_TEXT_REVERSE | CELL_TEXT_PAPER)

_Static_assert(!((CELL_TEXT_CODE | CELL_TEXT_SET | CELL_TEXT_WRITTEN | CELL_TEXT_GUARD) &
                 CELL_TEXT_SHOWS),
               "a cell's text and whether it shows are bits of their own");

// A fill: the cell. An erase is a blank with the pen's paper.
typedef struct
{
    cell_t cell;
    bool selective;
} cell_fill_t;

// The blank a shift brings in: a space, with the pen's paper or none.
typedef struct
{
    cell_t cell;
} cell_side_t;

typedef struct
{
    cell_t cell[CELL_STRIP_MAX];
} cell_strip_t;

// A side in the savestate: its cell.
#define CELL_SIDE_SST_SIZE 2

/* ---- fills and sides --------------------------------------------------- */

// The pen's background is other than the default as it is drawn: in iCE
// colors mode (?33) SGR 5/6 brightens the default background.
static inline bool cell_pen_paper(const term_t *term, const term_pen_t *pen)
{
    return pen->bg.kind != TERM_COLOR_DEFAULT ||
           (term->ice_colors && (pen->sgr_attr & TERM_ATTR_ANY_BLINK));
}

// An erase takes the pen's background, not reversed, and none of its
// attributes.
static inline cell_side_t cell_side_pen(const term_t *term)
{
    const cell_side_t side = {cell_pen_paper(term, term_pen(term)) ? CELL_TEXT_PAPER : 0};
    return side;
}

static inline cell_side_t cell_side_default(void)
{
    const cell_side_t side = {0};
    return side;
}

static inline bool cell_side_same(const cell_side_t *a, const cell_side_t *b)
{
    return a->cell == b->cell;
}

static inline cell_fill_t cell_blank(uint8_t code, cell_side_t side, bool selective,
                                     bool written)
{
    const cell_fill_t e = {(cell_t)(side.cell | code | (written ? CELL_TEXT_WRITTEN : 0)),
                           selective};
    return e;
}

// The cell holds whether it is written.
static inline bool cell_fill_same(const cell_fill_t *a, const cell_fill_t *b)
{
    return a->selective == b->selective && a->cell == b->cell;
}

// The cell a character lands as: its text and the pen's lines, swap and
// paper. A concealed cell draws its foreground as its background, so with
// its colors swapped its paper is the pen's foreground, which is never the
// default background. A cell written while DECSCA or SPA protects is
// guarded, and every one is written (cell.h). The width does not change the
// cell.
static inline cell_fill_t cell_from_pen(const term_t *term, uint8_t width, uint8_t code,
                                        unsigned set)
{
    (void)width;
    const term_pen_t *pen = term_pen(term);
    unsigned cell = code | (set == CELL_SET_ITALIC ? CELL_SET_PAGE : set) << CELL_TEXT_SET_SHIFT |
                    CELL_TEXT_WRITTEN;
    if (pen->sgr_attr & TERM_ATTR_UL_MASK)
        cell |= CELL_TEXT_UNDERLINE;
    if (pen->sgr_attr & (TERM_ATTR_STRIKE | TERM_ATTR_OVERLINE))
        cell |= CELL_TEXT_LINES;
    if (pen->reverse)
        cell |= CELL_TEXT_REVERSE;
    if (cell_pen_paper(term, pen) || (pen->reverse && pen->conceal))
        cell |= CELL_TEXT_PAPER;
    if (term_protecting(term))
        cell |= CELL_TEXT_GUARD;
    const cell_fill_t e = {(cell_t)cell, false};
    return e;
}

/* ---- cells ------------------------------------------------------------- */

static inline bool cell_guarded(const cell_t *cell)
{
    return *cell & CELL_TEXT_GUARD;
}

static inline void cell_put1(cell_t *at, const cell_fill_t *e)
{
    *at = e->cell;
}

// Runs of cells, out of line (cell_text.c).
void cell_run_fill(cell_t *dst, unsigned n, const cell_fill_t *e);
void cell_run_fill_sel(cell_t *row, unsigned from, unsigned to, const cell_fill_t *e);
void cell_run_move(cell_t *dst, const cell_t *src, unsigned n);
void cell_wipe(cell_t *row, unsigned from, unsigned to, bool selective);
void cell_marks(cell_t *row, unsigned from, unsigned to, unsigned set, unsigned clear,
                unsigned toggle);
void cell_strip_keep(cell_strip_t *strip, const cell_t *row, unsigned from, unsigned end,
                     unsigned width);
void cell_strip_put(const cell_strip_t *strip, cell_t *row, unsigned from, unsigned end,
                    unsigned width);
void cell_strip_move(cell_t *to, const cell_t *row, unsigned from, unsigned end,
                     unsigned width);

static inline bool cell_shows(const cell_t *cell)
{
    return (*cell & CELL_TEXT_CODE) != ' ' || (*cell & CELL_TEXT_SHOWS);
}

static inline uint16_t cell_text(const cell_t *cell)
{
    return (uint16_t)(*cell & (CELL_TEXT_CODE | CELL_TEXT_SET | CELL_TEXT_WRITTEN |
                               CELL_TEXT_GUARD));
}

/* ---- savestate --------------------------------------------------------- */

// A cell names a set there is.
static inline bool cell_ok(cell_t cell)
{
    return (cell & CELL_TEXT_SET) >> CELL_TEXT_SET_SHIFT < CELL_SET_ITALIC;
}

static inline void cell_side_put(sst_cursor_t *c, const cell_side_t *side)
{
    sst_put_u16(c, side->cell);
}

static inline bool cell_side_get(sst_cursor_t *c, cell_side_t *side)
{
    side->cell = sst_get_u16(c);
    return cell_ok(side->cell);
}

#endif /* _CORE_TERM_CELL_TEXT_H_ */
