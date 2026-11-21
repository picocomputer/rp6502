/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_CELLS_STORE_H_
#define _CORE_TERM_CELLS_STORE_H_

/* The cell store (cells.c) as draw.c sees it: each size's grid and the cells
 * of its screens, laid out as mode 0 draws them. Only the two files that make
 * the console's screen include this. Everything else reaches the store
 * through the hooks in cells.h. A screen's rows are laid out in its ring
 * (screen.h), and the store reaches a screen's cells through the screen. */

#include "core/sys/sst.h"
#include "core/term/cells.h"
#include "core/term/grid.h"
#include "core/vga/mode/mode0.h"
#include <stdbool.h>
#include <stdint.h>

// A fill owed to a row: the cell every cell of the row becomes, and its
// colors. An erase is a fill with a blank cell, DECALN's is a fill with an
// E, and REP's is a fill with its glyph as the pen draws it. A selective
// erase leaves the guarded cells alone.
typedef struct
{
    mode0_cell_t cell;
    mode0_colors_t colors;
    bool selective;
} cells_fill_t;

// A shift owed to a row, SL's and SR's: the cells that survive it, where
// they land, and the blanks either side of them. The survivors are the
// cells that land on columns [to, end), and the first of them is at column
// from until the row is paid. The columns left of to are blank in the left
// colors and those from end to cols in the right colors. Another shift of
// the row only changes the record.
typedef struct
{
    /* On a word, so the record is copied a word at a time. */
    _Alignas(4) uint8_t from;
    uint8_t to;
    uint8_t end;
    uint8_t cols;
    uint16_t left_fg;
    uint16_t left_bg;
    uint16_t right_fg;
    uint16_t right_bg;
} cells_shift_t;

// A change of attributes owed to a row, DECCARA's and DECRARA's: the
// columns [from, to), and the CELLS_MARK_ attributes to set, to clear, and
// then to reverse.
typedef struct
{
    _Alignas(4) uint8_t from;
    uint8_t to;
    uint8_t set;
    uint8_t clear;
    uint8_t toggle;
} cells_marks_t;

// What a row is owed: nothing, a fill, a shift, or a change of attributes.
enum
{
    CELLS_OWED_NONE,
    CELLS_OWED_FILL,
    CELLS_OWED_SHIFT,
    CELLS_OWED_MARKS,
};

typedef union
{
    cells_fill_t fill;
    cells_shift_t shift;
    cells_marks_t marks;
} cells_owed_t;

// A scroll between left and right margins owed to a screen: columns
// [from, end) of rows [top, bot] move n rows up or down, and this blank
// comes in. n is 0 when no scroll is owed. Another scroll of the same
// columns the same way only adds to n, so a run of line feeds moves the
// cells once.
typedef struct
{
    uint8_t top, bot, from, end;
    uint8_t n;
    bool up;
    cells_fill_t in;
} cells_later_t;

// The cells of a screen and what its rows are still owed. A row of the
// screen is in a row of memory, which the screen's ring gives (screen.h). A
// fill or a shift is owed to the row of memory, so it goes where the row
// goes and a scroll moves none of them. The screen has TERM_SPARE_ROWS more
// rows of memory than slots: the ring's spare is the one in no slot, which
// a smooth scroll gives the row coming in while the row going out still
// shows. cancels goes up, on both screens of a grid, at each scroll, index,
// clear and change of height, which end a smooth scroll of the grid, and at
// nothing else.
typedef struct screen_cells
{
    mode0_cell_t *mem;
    uint8_t width;
    uint8_t owed[TERM_MEM_ROWS];
    cells_owed_t what[TERM_MEM_ROWS];
    cells_later_t later;
    bool all_clean;
    uint32_t cancels;
} screen_cells_t;

// The terminal at one size, the rings of its screens and the line
// attributes of their rows of memory, and their cells: the primary's, then
// the alternate's, which a build without TERM_ALT_SCREEN does not give its
// screen.
typedef struct
{
    term_grid_t grid;
    uint8_t lines[2 * TERM_MEM_ROWS];
    uint8_t row_idx[2 * TERM_MAX_HEIGHT];
    screen_cells_t bufs[2];
} cells_t;

extern cells_t cells_40;
extern cells_t cells_80;

/* The store moved cells in a way that may change what shows, outside the
 * hooks that change a cell: a scroll it was owed is done. draw.c publishes
 * and clears it. */
extern bool cells_moved;

// The cells of row y of a screen that has them.
static inline mode0_cell_t *cells_row(const screen_t *s, uint8_t y)
{
    const screen_cells_t *c = s->cells;
    return c->mem + (uint32_t)screen_mem(s, y) * c->width;
}

// Each screen's cells are mode 0's, and each size's width is set.
void cells_store_init(void);

// New grids at both sizes, their screens laid out in the store's line sizes
// and rings and given its cells.
void cells_store_grids(void);

// Every cell of the store, the scrollback too, is a blank in the default
// colors at once.
void cells_store_blank(void);

// Do a scroll of between left and right margins that a screen of the grid
// is owed, or else pay one row of a screen what it is owed, the screen
// showing first.
void cells_clean_task(term_grid_t *g);

/* A screen's layout in the savestate: its ring and spare row, the line
 * attributes of each row of memory, what each row of memory is owed in a
 * record of 12 bytes, and the rest. A put first does
 * a scroll the screen is owed, so the layout saved is the layout the cells
 * are in. For a screen of the store's, with cells. */
#define CELLS_BUF_SST_SIZE (TERM_MAX_HEIGHT + 1 + 14 * TERM_MEM_ROWS + 3)
void cells_put_buf(sst_cursor_t *c, screen_t *s);
bool cells_get_buf(sst_cursor_t *c, screen_t *s);

#endif /* _CORE_TERM_CELLS_STORE_H_ */
