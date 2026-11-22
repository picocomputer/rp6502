/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_CELL_H_
#define _CORE_TERM_CELL_H_

/* What a cell of the cell store (screen.c) holds, which is the machine's
 * kind of cell. The store moves rows and runs of cells, and it reads or
 * writes what is in a cell only through the kind. There is one kind: mode 0's (cell_mode0.h), the cell as mode 0
 * draws it, with its colors.
 *
 * A kind gives the store these:
 *
 * cell_t: a cell, and with it whatever goes where the cell goes.
 * cell_fill_t: a fill, the cell every cell of a run becomes, and whether
 *   it is selective, which leaves the guarded cells. Its cell is .cell and
 *   its flag .selective; the rest is the kind's.
 * cell_side_t: the colors of a blank, the pen's or the default's.
 * cell_strip_t: the cells either side of some columns of a row,
 *   CELL_STRIP_MAX at most, kept while a margin scroll moves the rows.
 *
 * cell_from_pen(t, width, code, set): the fill of a character of a screen
 *   of width columns in the pen, code in the set (CELL_SET_).
 * cell_side_pen(t), cell_side_default(): the pen's blank and the default
 *   colors' blank.
 * cell_blank(code, side, selective, written): the fill of a blank, a space
 *   or DECALN's E, as side has it, written or not (CELL_TEXT_WRITTEN).
 * cell_fill_same: two that leave a row the same, whether the cells are
 *   written included.
 * cell_guarded(cell): DECSCA or SPA guards it.
 * cell_put1(at, fill): one cell written.
 * cell_run_fill(at, n, fill): n cells become the fill's cell, guarded or
 *   not.
 * cell_run_fill_sel(row, from, to, fill): columns [from, to) of a row
 *   written but for the guarded cells.
 * cell_run_move(to, from, n): n cells moved; the two may overlap.
 * cell_wipe(row, from, to, selective): the glyphs a space and the rest
 *   kept, and the cells written.
 * cell_marks(row, from, to, set, clear, toggle): DECCARA's and DECRARA's
 *   SCREEN_MARK_ attributes changed, and with SCREEN_MARK_WRITTEN set the
 *   cells written.
 * cell_strip_keep, cell_strip_put, cell_strip_move: the cells either side
 *   of columns [from, end) of a row of width cells kept, put back, or moved
 *   to another row.
 * cell_shows(cell): the cell shows something on the screen.
 * cell_text(cell): the cell's text, a CELL_TEXT_ word, the same on every
 *   kind. */

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
// DECSCA or SPA guards it.
//
// A cell is written as xterm's CHARDRAWN has it, so a copy of a row ends at
// its last written cell, as xterm's does. A character makes its cell
// written: a glyph, REP, DECFRA and DECALN's E. So do DECERA and DECSERA,
// a DECCRA on every cell it copies to, a DECCARA or DECRARA that changes a
// rectangle (DECSACE 2), and the blanks that ICH, IRM, SR, DECIC and DECBI
// bring in. An erase that ends short of the screen's last column, as EL 1
// and ECH may, makes its blanks written; one to the end of the row, EL 0
// and EL 2, does not. Nothing else does: the blanks that DCH, SL, DECDC and
// DECFI bring in, the rows a scroll or an erase of whole rows brings in,
// and a clear or a reset all leave cells that were never written. A
// selective erase is the erase it is, because a guarded cell is a written
// one and a copy only reads where a row's written cells end.
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
