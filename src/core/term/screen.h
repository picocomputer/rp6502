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

// The cells of a screen that has them.
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

/* ---- the selection ------------------------------------------------------
 *
 * The console's one selection, kept in the terminal's state on every chip
 * (vterm.h), because the store is what drops its highlight. It is on a
 * screen of one of the console's grids, from row of memory start_row,
 * column start_col, up to row of memory end_row, column end_col, that cell
 * not taken, and between its sides: no row of it takes a column before
 * left, but its first from its start, nor one from right on, but its last
 * up to its end. The mouse's sides are the screen's edges
 * (SCREEN_SELECTION_EDGE); the line editor's are the columns its line has,
 * between the left and right margins it was laid out in. Its columns are
 * each row's own, half the grid's width on a row of double width. It is
 * kept in rows of memory, which never move, so a
 * scroll that keeps it never changes it: the scroll only changes which row
 * of the screen each of its rows shows on. rows has a bit for each row of
 * memory it takes: its first, its last and those the screen shows between
 * them, all of them rows of the grid's height. The ghost of a smooth scroll
 * and the spare row are never among them, since a selected row going out
 * drops it.
 *
 * Its highlight shows until the user ends it or a change to its cells
 * drops it, as on xterm: a glyph, an erase, an insert or a delete that
 * reaches its columns; a job of any kind but SCREEN_JOB_ATTRS that does; a
 * selected row leaving the rows of the height; a scroll of some of the
 * screen's rows that holds some of its rows and not all, which would open
 * a gap in it; one of its rows taking another line size; a change of the
 * grid's height; a clear or a reset of its screen. A change of attributes
 * alone, DECCARA's and DECRARA's, keeps it. A job is judged as it starts,
 * before it changes a cell, so a chip that does a job over passes of its
 * main loop drops the selection at the same byte as one that does it at
 * once. A selection whose start is its end is none. */
typedef struct
{
    uint64_t rows;                // a bit for each row of memory it takes
    const screen_t *screen;       // the screen it was made on
    uint8_t start_row, start_col; // where it starts: a row of memory, a column
    uint8_t end_row, end_col;     // and where it ends, that cell not taken
    uint8_t left, right;          // its sides: the columns [left, right) its rows take
    bool shows;                   // its highlight shows: made, and not dropped
    bool changed;                 // since mode 0's row words were last written
    bool unsent;                  // a change of the user's the RIA has not sent
} screen_selection_t;
extern screen_selection_t vterm_selection;

_Static_assert(TERM_MEM_ROWS <= 64, "a bit of the selection for each row of memory");

// A right side past every row's last column: the screen's right edge.
#define SCREEN_SELECTION_EDGE 0x7F
_Static_assert(80 < SCREEN_SELECTION_EDGE, "the edge past the widest row");

// The columns [*from, *to) row of memory m of the selection takes, a row of
// cols columns.
static inline void screen_selection_cols(uint8_t m, unsigned cols, unsigned *from, unsigned *to)
{
    const screen_selection_t *v = &vterm_selection;
    const unsigned right = v->right < cols ? v->right : cols;
    *from = m == v->start_row ? v->start_col : v->left;
    *to = m == v->end_row ? v->end_col : right;
}

// Whether the selection shows on s: the first test of every drop, so the
// cost of one where there is none is a load and a compare.
static inline bool screen_selection_on(const screen_t *s)
{
    return vterm_selection.shows && s == vterm_selection.screen;
}

// The highlight goes, and the ends stay as they were.
static inline void screen_selection_drop(void)
{
    vterm_selection.shows = false;
    vterm_selection.changed = true;
}

// Columns [from, to) of row of memory m of the screen the selection shows
// on change: it drops when it takes one of them.
void screen_selection_touch(uint8_t m, unsigned from, unsigned to);

/* ---- the marks ----------------------------------------------------------
 *
 * The points the console's mouse holds on a screen's rows while it selects
 * (vterm_mouse.c), kept in step by the store because the store is what
 * moves rows, as it judges the selection. A mark is a row of memory, a
 * column of that row's own, and where in that column's cell the point is,
 * so a move of rows that keeps its row keeps it on its text. Where in the
 * cell is 0 for the cell's left edge, the gap before it, or 1 to 16 for a
 * point inside it, that many sixteenths of the cell's width from its left
 * edge, less one, so the place holds when the row changes width. Every
 * mark is on a row of the height, always: a row that leaves the rows of
 * the height takes its marks to the edge it left by, as xterm's
 * ScrollSelection holds its points there. A mark going out of the top, or
 * of a scrolling region's top, goes to the left edge of the first cell of
 * the row that is there after the move; one going out of the bottom, or of
 * a region's bottom, goes one past the last cell of the row that is there.
 * A clear of the ring keeps each mark on its row of the screen. The marks
 * are on a screen with cells, or on none. */
#define SCREEN_MARK_SUBS 16
typedef struct
{
    uint8_t row, col;
    uint8_t sub; // 0 the cell's left edge, or 1 + sixteenths into it
} screen_mark_t;

typedef struct
{
    const screen_t *screen; // the screen they are on, or NULL for none
    screen_mark_t *at;      // the owner's
    uint8_t n;              // how many, no more than 8
} screen_marks_t;
extern screen_marks_t vterm_marks;

// Whether the marks are on s: the first test of every move of the marks,
// never on a machine with none (TERM_MARKS).
static inline bool screen_marks_on(const screen_t *s)
{
    return TERM_MARKS && s == vterm_marks.screen;
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
// wrapped. A selected row that changes size drops the selection.
static inline void screen_set_size(screen_t *s, uint8_t y, uint8_t size)
{
    const uint8_t m = screen_mem(s, y);
    uint8_t *line = &s->line[m];
    if (screen_selection_on(s) && (*line & SCREEN_LINE_SIZE) != size)
        screen_selection_touch(m, 0, UINT8_MAX);
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
// A screen with a ring has a row of memory more than slots, the spare row,
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

// Rows [top, bot] move n rows up or down round the ring: each takes the
// row of memory n rows below it, or above it, and the n at the end go
// round, their line attributes with them, to be the n rows coming in,
// which are blank lines (screen_lines_blank). Returns the
// row of memory of the row that went out at the edge, which lands where
// the rows coming in start. With keep, for a smooth scroll of one row, the
// spare row of memory takes that slot instead, before anything erases the
// row coming in, so the row that went keeps its line attributes and its
// cells while it still shows going. A screen with no ring keeps nothing.
uint8_t screen_ring_scroll(screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up,
                           bool keep);

// The whole screen of height rows moves a row up, row 0 joining the rows
// above it, or a row down, the newest of those coming back. The row coming
// in is the one that was past the other end, and is a blank line. Returns
// the row of memory of the row that went: up, the newest row above row 0;
// down, the row just past the last. With keep, when that is the row coming
// in, which it is when the ring has no slot to spare, the spare row of
// memory takes its slot, as in screen_ring_scroll. Otherwise the row that
// went is in a slot past the screen's rows, which nothing but a REP's job
// writes until the next move of the ring: up, the newest row above row 0
// is one of the REP's rows when its glyphs filled it (screen_rep_t).
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
 * height move it, and the rest leave it be. A fill of more than one row,
 * an erase of rows, a rectangle, DECALN, a clear and the rows a scroll
 * brings in, is a job (below), as is a wipe, a change of attributes, a
 * shift, a scroll between left and right margins, a DECCRA or a REP of
 * more than one row. */

// n copies of the glyph at row y from column x, in the pen and the active
// character set. n never runs past the row's columns.
void screen_glyphs(term_t *t, screen_t *s, uint8_t y, uint8_t x, uint8_t byte, unsigned n);

// DECFRA: columns [left, right] of rows [top, bot] become the glyph, in the
// pen and the active character set: fewer columns on a double-size line,
// and none past them.
void screen_fill_rect(term_t *t, screen_t *s, uint8_t top, uint8_t bot, uint8_t left,
                      uint8_t right, uint8_t byte);

// DECERA: columns [left, right] of rows [top, bot] erased to written
// blanks, protected cells too, with the columns of a rectangle as
// screen_fill_rect has them.
void screen_erase_rect(term_t *t, screen_t *s, uint8_t top, uint8_t bot, uint8_t left,
                       uint8_t right);

// Erase columns [from, to) of row y to blanks written or not (cell.h). A selective erase
// leaves the protected cells.
void screen_erase(term_t *t, screen_t *s, uint8_t y, unsigned from, unsigned to,
                  bool selective, bool written);

// DECSERA: the glyphs of columns [left, right] of rows [top, bot] become
// spaces, with the columns of a rectangle as screen_fill_rect has them, and
// the cells keep their colors and attributes, as xterm has it, and are
// written. A selective one leaves the protected cells.
void screen_wipe_rect(screen_t *s, uint8_t top, uint8_t bot, uint8_t left, uint8_t right,
                      bool selective);

// Insert n erased cells at column x of row y, the cells up to column end
// moving right and off it; or delete n there, the cells up to end moving
// left and erased cells coming in before it. n is at most end - x. As in
// xterm, the cells an insert brings in are written and those a delete
// brings in are not.
void screen_insert(term_t *t, screen_t *s, uint8_t y, uint8_t x, unsigned end, unsigned n);
void screen_delete(term_t *t, screen_t *s, uint8_t y, uint8_t x, unsigned end, unsigned n);

// SL, SR, DECIC, DECDC, DECBI and DECFI: columns [from, end) of rows
// [top, bot], fewer on a double-size line and none past its columns, move
// n columns left or right, erased cells coming in at the other side,
// written when they come in at the left, as screen_insert's are, and not
// at the right.
void screen_shift(term_t *t, screen_t *s, uint8_t top, uint8_t bot, unsigned from,
                  unsigned end, unsigned n, bool left);

// DECCRA: the h rows of w columns at row sy, column sx are copied to row
// dy, column dx, colors, attributes and all, and every cell copied to is
// written, as xterm has it. A double-size line gives and takes only the
// columns it has. The two rectangles may overlap.
void screen_copy_rect(screen_t *s, uint8_t sy, uint8_t sx, uint8_t dy, uint8_t dx, uint8_t h,
                      uint8_t w);

// DECCARA and DECRARA: columns [left, right] of rows [top, bot] have these
// attributes set, cleared, then reversed, whatever else the cells hold,
// with the columns of a rectangle as screen_fill_rect has them.
// SCREEN_MARK_WRITTEN set makes the cells written; SCREEN_MARK_ALL does not
// include it.
#define SCREEN_MARK_BOLD 1u
#define SCREEN_MARK_UNDERLINE 2u
#define SCREEN_MARK_BLINK 4u
#define SCREEN_MARK_REVERSE 8u
#define SCREEN_MARK_ALL 15u
#define SCREEN_MARK_WRITTEN 16u
void screen_attrs(screen_t *s, uint8_t top, uint8_t bot, uint8_t left, uint8_t right,
                  unsigned set, unsigned clear, unsigned toggle);

// Erase rows [from, to), every column of their rows of memory, selectively
// or not, to blanks never written.
void screen_erase_rows(term_t *t, screen_t *s, uint8_t from, uint8_t to, bool selective);

// DECALN: the screen's height rows erased to written E's in the default
// colors.
void screen_align(term_t *t, screen_t *s, uint8_t height);

// REP's rows after the cursor's (screen_rep), done once the grid has
// written the cursor's own row and turned the ring for the scrolls the
// glyphs make. Rows [first, last] go down the screen, counted round the
// ring, so first may be a row above row 0 (screen_row_text). Each takes
// the glyph over columns [left, right], fewer on a double-size line, but
// for two things. Between left and right margins, which move cells in
// place of the ring, the first moves rows take the cells by rows below
// them, as a scroll of that many rows moves them, and the glyph comes in
// after them. The last row takes the glyph only up to column to, and then
// tail: the rest kept, or erased as the row came in with a scroll, or in
// insert mode pushed right past the glyphs. ghost is the row of memory the
// last scroll kept going out for a smooth scroll (screen_ring_index), when
// it was a row of the glyph, and mem_rows otherwise; one kept aside from
// the ring takes the glyph too.
enum
{
    SCREEN_REP_KEEP,   // the last row's cells past to stay
    SCREEN_REP_ERASE,  // they are erased in the pen's colors, never written
    SCREEN_REP_INSERT, // they move right past the glyphs and off the row
};

typedef struct
{
    uint8_t first, last;
    uint8_t left, right;
    uint8_t moves, by;
    uint8_t to;
    uint8_t tail; // SCREEN_REP_
    uint8_t ghost;
} screen_rep_t;

// REP's rows, as screen_rep_t has them, in the glyph byte as the pen and
// the active character set draw it.
void screen_rep(term_t *t, screen_t *s, uint8_t byte, const screen_rep_t *rep);

// Rows [top, bot] move n rows up or down, erased rows coming in. Returns
// the row of memory the row that went out at the edge is in. With keep, a
// smooth scroll of one row, that row keeps its cells while it still shows
// going, in a row of memory no slot of the screen has, when the screen has
// one to spare (screen_ring_scroll).
uint8_t screen_scroll(term_t *t, screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up,
                      bool keep);

// The same for only columns [from, end) of those rows, which are not the
// whole width: a scroll between left and right margins. The rows keep
// their line attributes, and nothing is kept.
void screen_scroll_cols(term_t *t, screen_t *s, uint8_t top, uint8_t bot, unsigned from,
                        unsigned end, uint8_t n, bool up);

// The whole screen of height rows moves a row up, the top row joining the
// scrollback, or a row down, the newest row of the scrollback lost. Returns
// the row of memory the row that went is in, which keep keeps as for
// screen_scroll (screen_ring_index).
uint8_t screen_index(term_t *t, screen_t *s, uint8_t height, bool up, bool keep);

// The screen erased in the pen's colors over its height rows, and its
// scrollback forgotten.
void screen_clear(term_t *t, screen_t *s, uint8_t height);

// RIS and the system's soft reset: the alternate screen erased in the
// default colors over its height rows, and its scrollback forgotten.
void screen_reset(screen_t *s, uint8_t height);

// The height of the screen showing went one row up or down to height. With
// top, the cursor was on the bottom row and the screen moved instead of the
// cursor: a grow brings a row back from the scrollback at the top, a shrink
// sends the top row to it. The grid then fits each of its screens.
void screen_height(screen_t *s, uint8_t height, bool grow, bool top);

// A screen of a grid whose height just changed to height: its scrollback
// is at most what the height leaves.
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

// Where screen_copy puts the text it takes: text for each character, a
// cell_text word, and line for each line end. A sink is the first member of
// whatever holds what it fills.
typedef struct screen_sink screen_sink_t;
struct screen_sink
{
    void (*text)(screen_sink_t *k, uint16_t word);
    void (*line)(screen_sink_t *k);
};

// The text of the screen from row y0, column x0 up to row y1, column x1,
// that cell not taken, as xterm copies it. The columns are the row's own,
// half the screen's width on a row of double width. A row's cells are
// taken up to its last written cell (cell.h), the cells never written
// before it as spaces. A line ends after each row but the last that did
// not wrap on its own, so a wrapped line joins the next with nothing
// between, as xterm's and Windows Terminal's copies do: the last row never
// ends a line, so a copy takes a row's line end only by going on into the
// next row. The
// screen has no job pending, and y1 is a row of the grid's height. The
// screen does not know that height, so it asserts only that y1 is a row of
// its ring.
void screen_copy(screen_t *s, uint8_t y0, uint8_t x0, uint8_t y1, uint8_t x1,
                 screen_sink_t *sink);

/* ---- inside the store ---------------------------------------------------
 *
 * The cells of a screen, of the machine's kind (cell.h), for screen.c and
 * for draw.c, which makes what mode 0 draws from them and saves them. */

/* A job: work on more than one row of a screen, the same work on each of
 * its rows, done a row at a time from its first row to its last, down the
 * screen, or with up up it. Each row's work is on columns [left, right]
 * of the row, and with sized only on those its line has, as a rectangle's
 * are: fewer on a double-size line, and none past them. A screen has one
 * job at most, and the terminal takes no byte while one is pending, so the
 * rows, their line sizes and the pen stay as they were when it started,
 * and nothing else the operation that starts it does changes its rows,
 * their line sizes or the ring after it. Its rows are rows of the
 * screen, not of memory, as nothing turns the ring while it runs, counted
 * round the ring, so a REP's can start above row 0. The work is one of
 * these: */
enum
{
    SCREEN_JOB_NONE,  // no job is pending
    SCREEN_JOB_FILL,  // every cell becomes the fill's (cell.h)
    SCREEN_JOB_MOVE,  // the cells of another row moved in (screen_job_move_t)
    SCREEN_JOB_SHIFT, // the cells moved sideways (screen_job_shift_t)
    SCREEN_JOB_ATTRS, // the attributes changed (screen_job_attrs_t)
    SCREEN_JOB_WIPE,  // the glyphs became spaces (screen_job_wipe_t)
    SCREEN_JOB_REP,   // REP's rows (screen_job_rep_t)
};

// A move: the job's first row takes the cells of row row from column col,
// and each row after it the cells of the row as far from that, so the
// moves of a scroll or a DECCRA read each row before they write it when
// the job's rows go the way the cells go. The first moves rows of the job
// are moved, and the rows after them take the fill in: the rows a scroll
// brings in. With written, every cell moved to is written (cell.h), as
// DECCRA's are. With sized, the cells moved stop at the source's line's
// columns too.
typedef struct
{
    uint8_t row, col;
    uint8_t moves;
    bool written;
    cell_fill_t in;
} screen_job_move_t;

// A shift: the cells move n columns left or right, as many as the row's
// columns hold, and the fill in comes in at the other side.
typedef struct
{
    uint8_t n;
    bool left;
    cell_fill_t in;
} screen_job_shift_t;

// A change of attributes, DECCARA's and DECRARA's: the SCREEN_MARK_
// attributes to set, to clear, and then to reverse.
typedef struct
{
    uint8_t set, clear, toggle;
} screen_job_attrs_t;

// A wipe, DECSERA's: the glyphs become spaces, the cells keep their colors
// and attributes and are written (cell_wipe), and with selective the
// guarded cells are left.
typedef struct
{
    bool selective;
} screen_job_wipe_t;

// REP's rows, as screen_rep_t has them: the glyph's fill, and the erase
// the last row's cells past to take with SCREEN_REP_ERASE.
typedef struct
{
    cell_fill_t glyph, erase;
    uint8_t moves, by;
    uint8_t to;
    uint8_t tail;
} screen_job_rep_t;

typedef struct
{
    uint8_t kind;        // SCREEN_JOB_
    uint8_t first, last; // the rows, in the order they are done
    uint8_t next;        // the row done next
    uint8_t left, right; // the columns
    bool up;             // the rows go up the screen
    bool sized;          // a row's columns stop at its line's
    union
    {
        cell_fill_t fill;
        screen_job_move_t move;
        screen_job_shift_t shift;
        screen_job_attrs_t attrs;
        screen_job_wipe_t wipe;
        screen_job_rep_t rep;
    };
} screen_job_t;

// The cells of a screen. A row of the screen is in a row of memory, which
// the screen's ring gives. The screen has a row of memory more than slots:
// the ring's spare is the one in no slot, which a smooth scroll gives the
// row coming in while the row going out still shows. Its job is the one
// pending, if any.
typedef struct screen_cells
{
    cell_t *mem;
    uint8_t width;
    screen_job_t job;
} screen_cells_t;

// The cells of row of memory m of a screen that has them.
static inline cell_t *screen_mem_row(const screen_t *s, uint8_t m)
{
    const screen_cells_t *c = s->cells;
    return c->mem + (uint32_t)m * c->width;
}

// The cells of row y of a screen that has them.
static inline cell_t *screen_row(const screen_t *s, uint8_t y)
{
    return screen_mem_row(s, screen_mem(s, y));
}

// n cells from cells are blanks in the default colors.
void screen_blank_cells(cell_t *cells, unsigned n);

/* ---- jobs --------------------------------------------------------------- */

// Whether the screen has a job pending. Never for a screen with no cells.
static inline bool screen_job_pending(const screen_t *s)
{
    return s->cells && s->cells->job.kind != SCREEN_JOB_NONE;
}

// The screen, which has no job pending, starts this one. It runs to the
// end at once, inside the operation that starts it, or with TERM_JOB_ROWS
// (term.h) is left pending for the machine's main loop. A job of one row
// is that row's work done at once, as any change to one row is: only work
// on more than one row is ever a job, and draw_job_started (draw.h) hears
// of it. A screen with no cells has no work to do.
void screen_job_start(screen_t *s, const screen_job_t *job);

// The next rows, up to rows of them, of the job the screen has pending,
// which has cells. True while some of the job is still to do.
bool screen_job_step(screen_t *s, unsigned rows);

// The job the screen has pending, if it has one, runs to the end.
void screen_job_finish(screen_t *s);

/* A screen's layout in the savestate: its ring and spare row, the line
 * attributes of each row of memory, the slot of row 0 and the rows above
 * it. A put first finishes the screen's job, so no job is ever saved. For
 * a screen with cells. */
#define SCREEN_SST_SIZE (TERM_MAX_HEIGHT + 1 + TERM_MEM_ROWS + 2)
void screen_sst_put(sst_cursor_t *c, screen_t *s);
bool screen_sst_get(sst_cursor_t *c, screen_t *s);

#endif /* _CORE_TERM_SCREEN_H_ */
