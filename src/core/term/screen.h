/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_SCREEN_H_
#define _CORE_TERM_SCREEN_H_

/* One of a grid's two screens, the primary or the alternate: its cursor,
 * its saved cursor, and the ring its rows are laid out in, each row with its
 * line attributes. The grid holds what the two share. */

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
// room. A row of memory holds the row's line attributes, and its cells for
// a screen that has them, so they all go where the row goes. A screen with
// no row_idx has its rows of memory in the order of the slots, so a move of
// its rows moves the line attributes themselves.
typedef struct
{
    term_pos_t pos;            // the cursor
    term_pos_t saved;          // DECSC's position
    uint8_t rows;              // the slots of the ring, the most rows the grid has
    uint8_t mem_rows;          // the rows of memory, the spare row with them
    uint8_t y_offset;          // the slot of row 0
    uint8_t hidden;            // the rows above row 0 still in the ring
    uint8_t spare;             // the row of memory in no slot; mem_rows when there is none
    uint8_t *line;             // the line attributes of each row of memory, the owner's storage
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

// The line attributes of a row: bits 0-1 its size, TERM_LINE_.
#define SCREEN_LINE_SIZE 3u

// The line size of row of memory m.
static inline uint8_t screen_mem_size(const screen_t *s, uint8_t m)
{
    return (uint8_t)(s->line[m] & SCREEN_LINE_SIZE);
}

// The line size of row y of the screen. Every reader of a line's size goes
// through here, never through the byte, which has room for more.
static inline uint8_t screen_size(const screen_t *s, uint8_t y)
{
    return screen_mem_size(s, screen_mem(s, y));
}

// Row y of the screen takes a line size, TERM_LINE_.
static inline void screen_set_size(screen_t *s, uint8_t y, uint8_t size)
{
    uint8_t *line = &s->line[screen_mem(s, y)];
    *line = (uint8_t)((*line & ~SCREEN_LINE_SIZE) | size);
}

// Rows [from, to) of the screen are single size, as an erase leaves them.
void screen_lines_normal(screen_t *s, unsigned from, unsigned to);

// Every row of memory of the screen is single size, the rows above row 0
// and the spare row too.
void screen_all_normal(screen_t *s);

// A screen of rows slots, single size, in order. Its ring is in row_idx,
// rows bytes of the owner's, or NULL for a screen that will have no cells.
// A screen with a ring has TERM_SPARE_ROWS rows of memory more than slots,
// because only a ring moves a spare row round, so a ring is for a screen
// of no more than TERM_MAX_HEIGHT rows. Its line attributes are in line, a
// byte of the owner's for each row of memory. It has no cells. The cursors
// are left as they are.
void screen_init(screen_t *s, uint8_t rows, uint8_t *line, uint8_t *row_idx);

// The screen keeps its cells in cells, which has a row of memory for each
// of the screen's. Once, right after screen_init.
void screen_set_cells(screen_t *s, struct screen_cells *cells);

// The ring in order, with no rows above row 0 and the spare row the last.
// The line attributes stay with their rows of memory.
void screen_clear(screen_t *s);

// Rows [top, bot] turn n rows up or down round the ring: each takes the row
// of memory n rows below it, or above it, and the n at the end go round,
// their line attributes with them. Nothing else changes.
void screen_rotate(screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up);

// Rows [top, bot] move n rows up or down, as screen_rotate turns them, and
// the n rows coming in are single size. Returns the row of memory of the
// row that went out at the edge, which lands where the rows coming in
// start. With keep, for a smooth scroll of one row, the spare row of memory
// takes that slot instead, before anything erases the row coming in, so
// the row that went keeps its line size, its cells and what it is owed
// while it still shows going. A screen with no spare row keeps nothing.
uint8_t screen_scroll(screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up, bool keep);

// The whole screen of height rows moves a row up, row 0 joining the rows
// above it, or a row down, the newest of those coming back. The row coming
// in is the one that was past the other end, and is single size. Returns
// the row of memory of the row that went: up, the newest row above row 0;
// down, the row just past the last. With keep, when that is the row coming
// in, which it is when the ring has no slot to spare, the spare row of
// memory takes its slot, as in screen_scroll. Otherwise the row that went is
// in a slot past the screen's rows, which nothing writes until the next
// move of the ring.
uint8_t screen_index(screen_t *s, uint8_t height, bool up, bool keep);

// The height went one row up or down to height, and with top the screen
// moved instead of the cursor: a grow brings back the newest row above row
// 0 as it left, a shrink sends row 0 there. A grow at the bottom, or one at
// the top with no row to bring back, brings in a single-size row. True for
// the second, where the row that came in holds nothing of the screen's.
bool screen_height(screen_t *s, uint8_t height, bool grow, bool top);

// The rows above row 0 are at most what a height of height rows leaves.
void screen_fit(screen_t *s, uint8_t height);

// ED 3: the rows above row 0 are forgotten.
static inline void screen_forget(screen_t *s)
{
    s->hidden = 0;
}

#endif /* _CORE_TERM_SCREEN_H_ */
