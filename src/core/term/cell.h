/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_CELL_H_
#define _CORE_TERM_CELL_H_

/* What a cell of the cell store (screen.c) holds, which is the machine's
 * kind of cell. The store moves rows and runs of cells, and keeps what each
 * row is owed, and it reads or writes what is in a cell only through the
 * kind. There is one kind: mode 0's (cell_mode0.h), the cell as mode 0
 * draws it, with its colors.
 *
 * A kind gives the store these:
 *
 * cell_t: a cell, and with it whatever goes where the cell goes.
 * cell_fill_t: a fill, the cell every cell of a run becomes, and whether
 *   it is selective, which leaves the guarded cells. Its cell is .cell and
 *   its flag .selective; the rest is the kind's.
 * cell_side_t: the blank a shift brings in either side of the cells that
 *   survive it, the pen's at the time.
 * cell_strip_t: the cells either side of some columns of a row,
 *   CELL_STRIP_MAX at most, kept while a margin scroll moves the rows.
 *
 * cell_from_pen(t, width, code, set): the fill of a character of a screen
 *   of width columns in the pen, code in the set (CELL_SET_).
 * cell_side_pen(t), cell_side_default(): the pen's blank and the default
 *   colors' blank.
 * cell_blank(code, side, selective): the fill of a blank, a space or
 *   DECALN's E, as side has it.
 * cell_fill_same, cell_side_same: two that leave a row the same.
 * cell_guarded(cell): DECSCA or SPA guards it.
 * cell_put1(at, fill): one cell written.
 * cell_run_fill(at, n, fill): n cells become the fill's cell, guarded or
 *   not.
 * cell_run_fill_sel(row, from, to, fill): columns [from, to) of a row
 *   written but for the guarded cells.
 * cell_run_move(to, from, n): n cells moved; the two may overlap.
 * cell_wipe(row, from, to, selective): the glyphs a space and the rest kept.
 * cell_marks(row, from, to, set, clear, toggle): DECCARA's and DECRARA's
 *   SCREEN_MARK_ attributes changed.
 * cell_strip_keep, cell_strip_put, cell_strip_move: the cells either side
 *   of columns [from, end) of a row of width cells kept, put back, or moved
 *   to another row.
 * cell_shows(cell): the cell shows something on the screen.
 * cell_text(cell): the cell's text, a CELL_TEXT_ word, the same on every
 *   kind.
 * cell_fill_put, cell_fill_get, cell_side_put, cell_side_get: a fill or a
 *   side in the savestate, in CELL_FILL_SST_SIZE and CELL_SIDE_SST_SIZE
 *   bytes; a get says whether it is one. */

#include "core/term/term.h"
#include <stdint.h>

// Where a character's glyph is: the code page, DEC Special Graphics, DEC
// Technical, or the code page's in italics.
enum
{
    CELL_SET_PAGE,
    CELL_SET_DEC,
    CELL_SET_TECH,
    CELL_SET_ITALIC,
};

// A cell's text (cell_text): the character in bits 0-7, its set in bits
// 8-9, a CELL_SET_ other than CELL_SET_ITALIC, which is the code page's
// character drawn otherwise, whether the cell was written, and whether
// DECSCA or SPA guards it. No kind marks a cell written yet.
#define CELL_TEXT_CODE 0xFFu
#define CELL_TEXT_SET_SHIFT 8
#define CELL_TEXT_SET (3u << CELL_TEXT_SET_SHIFT)
#define CELL_TEXT_WRITTEN (1u << 13)
#define CELL_TEXT_GUARD (1u << 14)

// The most cells a strip keeps (cell_strip_t). With more cells either side
// of the columns than this, the store copies the cells between the columns
// instead (screen_settle).
#define CELL_STRIP_MAX 40

#if TERM_CELL_TEXT
#include "core/term/cell_text.h"
#else
#include "core/term/cell_mode0.h"
#endif

#endif /* _CORE_TERM_CELL_H_ */
