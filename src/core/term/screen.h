/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_SCREEN_H_
#define _CORE_TERM_SCREEN_H_

/* One of a grid's two screens, the primary or the alternate: its cursor,
 * its saved cursor, its line sizes, and the ring its rows are laid out in.
 * The grid holds what the two share. */

#include <stdbool.h>
#include <stdint.h>

// A cursor's place in a grid.
typedef struct
{
    uint8_t x, y;
    bool wrap_pending; // the last column was written; the next glyph wraps
} term_pos_t;

// The cells of a screen that has them, which are the cell store's
// (cells_store.h).
struct screen_cells;

// A screen's rows are in rows of memory, laid out in a ring of slots: row y
// of the screen is in slot y_offset + y, counted round the ring, and a slot
// holds the row of memory row_idx gives it. A scroll of the whole screen
// turns the ring through y_offset, and a scroll of some of its rows moves
// them round row_idx, so no row of memory moves. The rows that leave the
// top stay in the ring, the newest first, as long as the height leaves
// room. A screen with no row_idx has its rows of memory in the order of the
// slots, and a screen with no cells has nothing in its rows of memory, so a
// move of its rows moves only the ring.
typedef struct
{
    term_pos_t pos;            // the cursor
    term_pos_t saved;          // DECSC's position
    uint8_t rows;              // the slots of the ring, the most rows the grid has
    uint8_t mem_rows;          // the rows of memory, the spare row with them
    uint8_t y_offset;          // the slot of row 0
    uint8_t hidden;            // the rows above row 0 still in the ring
    uint8_t spare;             // the row of memory in no slot; mem_rows when there is none
    uint8_t *line;             // TERM_LINE_ for each row of the screen, the owner's storage
    uint8_t *row_idx;          // the row of memory in each slot, the owner's storage, or NULL
    struct screen_cells *cells; // its cells, or NULL for a screen with none
} screen_t;

// The slot row y of the screen is in. Unsigned, so a ring of up to 255
// slots never wraps the sum.
static inline uint8_t screen_slot(const screen_t *s, uint8_t y)
{
    unsigned slot = (unsigned)s->y_offset + y;
    return (uint8_t)(slot >= s->rows ? slot - s->rows : slot);
}

// The row of memory row y of the screen is in.
static inline uint8_t screen_mem(const screen_t *s, uint8_t y)
{
    const uint8_t slot = screen_slot(s, y);
    return s->row_idx ? s->row_idx[slot] : slot;
}

// A screen of rows rows, single size, in order, with its line sizes in line
// and its ring in row_idx, which may be NULL: both rows bytes of the
// owner's. It has no cells. The cursors are left as they are.
void screen_init(screen_t *s, uint8_t rows, uint8_t *line, uint8_t *row_idx);

// The screen keeps its cells in cells, which has TERM_SPARE_ROWS rows of
// memory more than the ring has slots. Once, right after screen_init.
void screen_set_cells(screen_t *s, struct screen_cells *cells);

// The ring in order, with no rows above row 0 and the spare row the last.
void screen_clear(screen_t *s);

// Rows [top, bot] move n rows up or down round row_idx: each takes the row
// of memory n rows below it, or above it, and the n at the end go round.
void screen_rotate(screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up);

// The whole screen of height rows moves a row up, row 0 joining the rows
// above it, or a row down, the newest of those coming back. The row coming
// in is the one that was past the other end.
void screen_index(screen_t *s, uint8_t height, bool up);

// The height went one row up or down, and with top the screen moved instead
// of the cursor: a grow brings back the newest row above row 0, a shrink
// sends row 0 there. True when a grow at the top had none to bring back, so
// the row that came in holds nothing of the screen's.
bool screen_height(screen_t *s, bool grow, bool top);

// The rows above row 0 are at most what a height of height rows leaves.
void screen_fit(screen_t *s, uint8_t height);

// ED 3: the rows above row 0 are forgotten.
static inline void screen_forget(screen_t *s)
{
    s->hidden = 0;
}

#endif /* _CORE_TERM_SCREEN_H_ */
