/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_GRID_H_
#define _CORE_TERM_GRID_H_

/* Between term.c and grid.c: what the terminal's parser hands each grid. The
 * parser decodes a byte and does everything that is the same at every size;
 * what moves the cursor or the rows, it hands to every grid with one of
 * these. The sequence's numbers are in the terminal's csi_param. */

#include "core/term/term.h"

typedef enum
{
    GRID_PRINT,   // ch, a glyph
    GRID_C0,      // ch, BS, HT, LF, VT, FF or CR
    GRID_CSI,     // ch, the final byte of a CSI with no private marker or intermediate
    GRID_REP,     // REP, the last glyph again
    GRID_SL,      // CSI SP @
    GRID_SR,      // CSI SP A
    GRID_ESC,     // ch, the final byte of ESC D, E, H or M
    GRID_LINE,    // ch, the final byte of ESC # 3 to 6 and 8
    GRID_VT52,    // ch, the letter of a VT52 cursor or erase sequence
    GRID_VT52_Y,  // ch, the column of VT52's ESC Y; the row is csi_param[0]
    GRID_SAVE,    // DECSC: the position
    GRID_RESTORE, // DECRC: the position
    GRID_HOME,    // the top left, or the region's under DECOM
    GRID_STAY,    // the cursor where it is, its row touched and its wrap ended
    GRID_ALT_IN,  // the alternate screen's cursor takes the primary's
    GRID_ALT_OUT, // the primary screen's cursor takes the alternate's
    GRID_CLEAR,   // the screen showing erased, every line single size
    GRID_RESET,   // RIS: the margins, the tabs and the saved positions
    GRID_DECSTR,  // DECSTR: the margins and the saved positions
} grid_op_t;

void grid_do(term_t *t, term_grid_t *g, grid_op_t op, uint8_t ch);

/* g becomes a copy of from, with its own line sizes in lines, rows a
 * screen. The caller points its cur and ls at the screen showing. */
void term_grid_copy(term_grid_t *g, const term_grid_t *from, uint8_t *lines, uint8_t rows);

#endif /* _CORE_TERM_GRID_H_ */
