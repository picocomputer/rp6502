/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_SCREEN_H_
#define _CORE_TERM_SCREEN_H_

/* One of a grid's two screens, the primary or the alternate: its cursor,
 * its saved cursor, the ring its rows are laid out in, each row with its
 * line attributes, and its cells for a screen that has them. The grid holds
 * what the two share. screen.c is the cell store: the grid (grid.c) moves a
 * screen's rows and changes its cells through the functions here, and
 * draw.c reads them for what mode 0 draws. */

#include "core/sys/sst.h"
#include "core/term/cell.h"
#include "core/term/term.h"
#include <stdbool.h>
#include <stdint.h>

// A cursor's place in a grid.
typedef struct
{
    uint8_t x, y;
    bool wrap_pending; // the last column was written; the next glyph wraps
} term_pos_t;

// The cells of a screen that has them, and what its rows are still owed.
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
    term_pos_t pos;             // the cursor
    term_pos_t saved;           // DECSC's position
    uint8_t rows;               // the slots of the ring, the most rows the grid has
    uint8_t mem_rows;           // the rows of memory, the spare row with them
    uint8_t y_offset;           // the slot of row 0
    uint8_t hidden;             // the rows above row 0 still in the ring
    uint8_t spare;              // the row of memory in no slot; mem_rows when there is none
    uint8_t *line;              // the line attributes of each row of memory, the owner's storage
    uint8_t *row_idx;           // the row of memory in each slot, the owner's storage, or NULL
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

// The line attributes of a row: bits 0-1 its size, TERM_LINE_, and bit 2
// TERM_LINE_WRAPPED.
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

// Row y of the screen takes a line size, TERM_LINE_, and keeps whether it
// wrapped.
static inline void screen_set_size(screen_t *s, uint8_t y, uint8_t size)
{
    uint8_t *line = &s->line[screen_mem(s, y)];
    *line = (uint8_t)((*line & ~SCREEN_LINE_SIZE) | size);
}

// Whether row y of the screen wrapped on its own, TERM_LINE_WRAPPED.
static inline bool screen_wrapped(const screen_t *s, uint8_t y)
{
    return s->line[screen_mem(s, y)] & TERM_LINE_WRAPPED;
}

// Row y of the screen wrapped on its own, or did not, and keeps its size.
static inline void screen_set_wrapped(screen_t *s, uint8_t y, bool wrapped)
{
    uint8_t *line = &s->line[screen_mem(s, y)];
    *line = (uint8_t)(wrapped ? *line | TERM_LINE_WRAPPED : *line & ~TERM_LINE_WRAPPED);
}

// Rows [from, to) of the screen are single size and keep whether they
// wrapped, as DECALN leaves them.
void screen_lines_normal(screen_t *s, unsigned from, unsigned to);

// Rows [from, to) of the screen come in blank: single size, and not
// wrapped, as a whole row erased or scrolled in is.
void screen_lines_blank(screen_t *s, unsigned from, unsigned to);

// Rows [from, to) of the screen are not wrapped and keep their sizes.
void screen_lines_unwrap(screen_t *s, unsigned from, unsigned to);

// Every row of memory of the screen is single size, the rows above row 0
// and the spare row too, and keeps whether it wrapped.
void screen_all_normal(screen_t *s);

// Every row of memory of the screen is blank, single size and not wrapped,
// the rows above row 0 and the spare row too.
void screen_all_blank(screen_t *s);

// A screen of rows slots, blank lines, in order. Its ring is in row_idx,
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

/* ---- the ring -----------------------------------------------------------
 *
 * What every screen's rows do, with cells or without. The store's moves
 * below do these first. */

// The ring in order, with no rows above row 0 and the spare row the last.
// The line attributes stay with their rows of memory.
void screen_ring_clear(screen_t *s);

// Rows [top, bot] turn n rows up or down round the ring: each takes the row
// of memory n rows below it, or above it, and the n at the end go round,
// their line attributes with them. Nothing else changes.
void screen_rotate(screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up);

// Rows [top, bot] move n rows up or down, as screen_rotate turns them, and
// the n rows coming in are blank lines (screen_lines_blank). Returns the
// row of memory of the row that went out at the edge, which lands where
// the rows coming in start. With keep, for a smooth scroll of one row, the
// spare row of memory takes that slot instead, before anything erases the
// row coming in, so the row that went keeps its line attributes, its cells
// and what it is owed while it still shows going. A screen with no spare
// row keeps nothing.
uint8_t screen_ring_scroll(screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up,
                           bool keep);

// The whole screen of height rows moves a row up, row 0 joining the rows
// above it, or a row down, the newest of those coming back. The row coming
// in is the one that was past the other end, and is a blank line. Returns
// the row of memory of the row that went: up, the newest row above row 0;
// down, the row just past the last. With keep, when that is the row coming
// in, which it is when the ring has no slot to spare, the spare row of
// memory takes its slot, as in screen_ring_scroll. Otherwise the row that
// went is in a slot past the screen's rows, which nothing writes until the
// next move of the ring.
uint8_t screen_ring_index(screen_t *s, uint8_t height, bool up, bool keep);

// The height went one row up or down to height, and with top the screen
// moved instead of the cursor: a grow brings back the newest row above row
// 0 as it left, a shrink sends row 0 there. A grow at the bottom, or one at
// the top with no row to bring back, brings in a blank line. True for the
// second, where the row that came in holds nothing of the screen's.
bool screen_ring_height(screen_t *s, uint8_t height, bool grow, bool top);

// The rows above row 0 are at most what a height of height rows leaves.
void screen_ring_fit(screen_t *s, uint8_t height);

// ED 3: the rows above row 0 are forgotten.
static inline void screen_forget(screen_t *s)
{
    s->hidden = 0;
}

/* ---- the cell store -----------------------------------------------------
 *
 * What the grid asks of the screen showing, or of the one it names. The
 * terminal has already changed its own state when the grid calls one, so
 * the cursor, the margins and the line sizes are the new ones. Colors come
 * from the pen, and an erased cell takes the pen's colors, not reversed. An
 * erased cell is one never written (cell.h) unless the function says it
 * is. A screen with no cells, a program's own size (term_add_grid) for one,
 * has only its ring: a scroll, an index, a clear, a reset and a change of
 * height move it, and the rest leave it be. */

// Row y is about to be written or have the cursor on it: a lazy erase still
// owed to it is done now.
void screen_touch(screen_t *s, uint8_t y);

// n copies of the glyph at row y from column x, in the pen and the active
// character set. n never runs past the row's columns, and the row is the
// cursor's or one the caller has touched.
void screen_glyphs(term_t *t, screen_t *s, uint8_t y, uint8_t x, uint8_t byte, unsigned n);

// Every cell of row y becomes the glyph, in the pen and the active character
// set, when the row is next touched or the store gets to it. The cursor's
// row is never left owing one.
void screen_fill_row(term_t *t, screen_t *s, uint8_t y, uint8_t byte);

// Erase columns [from, to) of row y, which is the cursor's or one the
// caller has touched, to blanks written or not (cell.h). A selective erase
// leaves the protected cells.
void screen_erase(term_t *t, screen_t *s, uint8_t y, unsigned from, unsigned to,
                  bool selective, bool written);

// DECSERA: the glyphs of columns [from, to) of row y, which the caller has
// touched, become spaces, and the cells keep their colors and attributes,
// as xterm has it, and are written. A selective one leaves the protected
// cells.
void screen_wipe(term_t *t, screen_t *s, uint8_t y, unsigned from, unsigned to,
                 bool selective);

// Insert n erased cells at column x of row y, the cells up to column end
// moving right and off it; or delete n there, the cells up to end moving
// left and erased cells coming in before it. n is at most end - x. As in
// xterm, the cells an insert brings in are written and those a delete
// brings in are not.
void screen_insert(term_t *t, screen_t *s, uint8_t y, uint8_t x, unsigned end, unsigned n);
void screen_delete(term_t *t, screen_t *s, uint8_t y, uint8_t x, unsigned end, unsigned n);

// SL and SR: columns [from, end) of rows [top, bot] move n columns left or
// right, erased cells coming in at the other side, written when they come
// in at the left, as screen_insert's are, and not at the right.
void screen_shift(term_t *t, screen_t *s, uint8_t top, uint8_t bot, unsigned from,
                  unsigned end, unsigned n, bool left);

// n cells of row sy from column sx are copied to row dy from column dx,
// colors, attributes and all, and every cell copied to is written, as
// xterm has it. The two runs may overlap.
void screen_copy_cells(term_t *t, screen_t *s, uint8_t sy, unsigned sx, uint8_t dy,
                       unsigned dx, unsigned n);

// DECCARA and DECRARA: columns [from, to) of row y have these attributes
// set, cleared, then reversed, whatever else the cells hold, when the row
// is next touched or the store gets to it. SCREEN_MARK_WRITTEN set makes
// the cells written; SCREEN_MARK_ALL does not include it.
#define SCREEN_MARK_BOLD 1u
#define SCREEN_MARK_UNDERLINE 2u
#define SCREEN_MARK_BLINK 4u
#define SCREEN_MARK_REVERSE 8u
#define SCREEN_MARK_ALL 15u
#define SCREEN_MARK_WRITTEN 16u
void screen_attrs(screen_t *s, uint8_t y, unsigned from, unsigned to, unsigned set,
                  unsigned clear, unsigned toggle);

// Lazy-erase rows [from, to), selectively or not, to blanks written or not.
void screen_erase_rows(term_t *t, screen_t *s, uint8_t from, uint8_t to, bool selective,
                       bool written);

// DECALN: the screen's height rows lazy-erased to written E's in the
// default colors.
void screen_align(term_t *t, screen_t *s, uint8_t height);

// Rows [top, bot] move n rows up or down, erased rows coming in. Returns
// the row of memory the row that went out at the edge is in. With keep, a
// smooth scroll of one row, that row keeps its cells while it still shows
// going, in a row of memory no slot of the screen has, when the screen has
// one to spare (screen_ring_scroll).
uint8_t screen_scroll(term_t *t, screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up,
                      bool keep);

// The same for only columns [from, end) of those rows, which are not the
// whole width: a scroll between left and right margins.
void screen_scroll_cols(term_t *t, screen_t *s, uint8_t top, uint8_t bot, unsigned from,
                        unsigned end, uint8_t n, bool up);

// The whole screen of height rows moves a row up, the top row joining the
// scrollback, or a row down, the newest row of the scrollback lost. Returns
// the row of memory the row that went is in, which keep keeps as for
// screen_scroll (screen_ring_index).
uint8_t screen_index(term_t *t, screen_t *s, uint8_t height, bool up, bool keep);

// The screen lazy-erased in the pen's colors over its height rows, and its
// scrollback forgotten.
void screen_clear(term_t *t, screen_t *s, uint8_t height);

// RIS and the system's soft reset: the alternate screen lazy-erased in the
// default colors over its height rows, and its scrollback forgotten.
void screen_reset(screen_t *s, uint8_t height);

// The height of the screen showing went one row up or down to height. With
// top, the cursor was on the bottom row and the screen moved instead of the
// cursor: a grow brings a row back from the scrollback at the top, a shrink
// sends the top row to it. The grid then fits each of its screens.
void screen_height(screen_t *s, uint8_t height, bool grow, bool top);

// A screen of a grid whose height just changed to height: its scrollback
// is at most what the height leaves, and the rows now in view may be owed
// something from before they left.
void screen_fit(screen_t *s, uint8_t height);

// Whether row y has anything on it: a cell that shows (cell_shows). No for
// a screen with no cells.
bool screen_row_used(screen_t *s, uint8_t y);

// The text of row y, a cell_text word (cell.h) for each of its columns,
// into out, which has room for the screen's width. Returns the columns, the
// screen's width or half of it on a row of double width; none for a screen
// with no cells. Row y may be a row above row 0 still in the ring, as row
// rows - 1 is the newest of them.
unsigned screen_row_text(screen_t *s, uint8_t y, uint16_t *out);

/* ---- inside the store ---------------------------------------------------
 *
 * The cells of a screen, of the machine's kind (cell.h), for screen.c and
 * for draw.c, which makes what mode 0 draws from them and saves them. */

// A shift owed to a row, SL's and SR's: the cells that survive it, where
// they land, and the blanks either side of them. The survivors are the
// cells that land on columns [to, end), and the first of them is at column
// from until the row is paid. The columns left of to are the left blank
// and those from end to cols the right one. Another shift of the row only
// changes the record.
typedef struct
{
    /* On a word, so the record is copied a word at a time. */
    _Alignas(4) uint8_t from;
    uint8_t to;
    uint8_t end;
    uint8_t cols;
    cell_side_t left;
    cell_side_t right;
} screen_shift_t;

// A change of attributes owed to a row, DECCARA's and DECRARA's: the
// columns [from, to), and the SCREEN_MARK_ attributes to set, to clear, and
// then to reverse.
typedef struct
{
    _Alignas(4) uint8_t from;
    uint8_t to;
    uint8_t set;
    uint8_t clear;
    uint8_t toggle;
} screen_marks_t;

// What a row is owed: nothing, a fill, a shift, or a change of attributes.
// A selective fill leaves the guarded cells alone.
enum
{
    SCREEN_OWED_NONE,
    SCREEN_OWED_FILL,
    SCREEN_OWED_SHIFT,
    SCREEN_OWED_MARKS,
};

typedef union
{
    cell_fill_t fill;
    screen_shift_t shift;
    screen_marks_t marks;
} screen_owed_t;

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
    cell_fill_t in;
} screen_later_t;

// The cells of a screen and what its rows are still owed. A row of the
// screen is in a row of memory, which the screen's ring gives. A fill or a
// shift is owed to the row of memory, so it goes where the row goes and a
// scroll moves none of them. The screen has TERM_SPARE_ROWS more rows of
// memory than slots: the ring's spare is the one in no slot, which a smooth
// scroll gives the row coming in while the row going out still shows.
typedef struct screen_cells
{
    cell_t *mem;
    uint8_t width;
    uint8_t owed[TERM_MEM_ROWS];
    screen_owed_t what[TERM_MEM_ROWS];
    screen_later_t later;
    bool all_clean;
} screen_cells_t;

/* The store moved cells in a way that may change what shows, outside the
 * functions that change a cell: a scroll it was owed is done, or the height
 * changed. draw.c publishes and clears it. */
extern bool screen_moved;

// The cells of row y of a screen that has them.
static inline cell_t *screen_row(const screen_t *s, uint8_t y)
{
    const screen_cells_t *c = s->cells;
    return c->mem + (uint32_t)screen_mem(s, y) * c->width;
}

// n cells from cells are blanks in the default colors.
void screen_blank_cells(cell_t *cells, unsigned n);

// Do the scroll between left and right margins the screen is owed, if it
// is owed one.
void screen_settle(screen_t *s);

// Pay one row of the screen's height rows what it is owed. True when a row
// was paid, false when none of them owes anything.
bool screen_tidy(screen_t *s, uint8_t height);

/* A screen's layout in the savestate: its ring and spare row, the line
 * attributes of each row of memory, what each row of memory is owed in a
 * record of SCREEN_SST_RECORD bytes, and the rest. A put first does a
 * scroll the screen is owed, so the layout saved is the layout the cells
 * are in. For a screen with cells. */
#define SCREEN_SST_RECORD 12
#define SCREEN_SST_SIZE (TERM_MAX_HEIGHT + 1 + (2 + SCREEN_SST_RECORD) * TERM_MEM_ROWS + 3)
void screen_sst_put(sst_cursor_t *c, screen_t *s);
bool screen_sst_get(sst_cursor_t *c, screen_t *s);

#endif /* _CORE_TERM_SCREEN_H_ */
