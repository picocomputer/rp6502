/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_CELLS_H_
#define _CORE_TERM_CELLS_H_

/* The console's screen on a machine that draws it: the terminal (term.h)
 * followed at 40 and 80 columns, fed every byte of the console, and the
 * cells, blinks and save state of both sizes. It writes what mode 0 draws
 * (core/vga/mode/mode0.h) for the size showing.
 *
 * The cell store takes more memory than anything else here, and two things
 * size it: TERM_ALT_SCREEN, and the machine's TERM_MAX_HEIGHT — 32 rows only
 * where the device's 512-line SXGA console exists, 30 everywhere else. */

#include "core/sys/sst.h"
#include "core/term/term.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void cells_init(void);
void cells_task(void);

/* The console's terminal. On a machine that runs a program and draws its
 * screen in one, it is this file's; the RIA has its own, which follows the
 * VGA's (ria/term/cells.c). */
extern term_t cells_term;

// RIS, and the system's soft reset, which also sets the 40-column terminal
// back to TERM_STD_HEIGHT: only a mode 0 on a 320-wide canvas sets its
// height, so after either chip restarts alone the two could differ.
void cells_RIS(void);
void cells_RIS_no_clear(void);

/* Writes mode 0's words for the size showing. cells_out_chars calls it
 * after each batch of the console's bytes, cells_sst_load after a load,
 * and cells_task after the blinks, a reset or a new height. Something else
 * that changes what shows calls it too: a savestate load that puts the
 * canvas back, or a test that feeds term_write directly. */
void cells_publish(void);

// The renderer reports its geometry: rows of the 40- or 80-column terminal.
void cells_set_height(uint8_t width, uint8_t height);

/* The chunk holds the terminal (term.h) and both sizes in full: each one's
 * grid, the layout of its screens' cells (the scroll remap and the fills
 * its rows are owed) and its blinks, then the cursor's color, the cells, the
 * colors of each, and the palette the cells' colors name: its 256
 * entries and the default foreground and background.
 *
 * The cells' addresses are never written, because they are mode 0's
 * (mode0_cells), the same in every build of a machine.
 *
 * The chunk starts with the terminal dimensions and a load rejects a chunk
 * whose dimensions differ, because a blob made at a taller height would put
 * its rows in the wrong places. */
#define CELLS_BUF_SST_SIZE (14 * TERM_MAX_HEIGHT + 3)
#define CELLS_ONE_SST_SIZE (TERM_GRID_SST_SIZE(TERM_MAX_HEIGHT) + 2 * CELLS_BUF_SST_SIZE + 18)
#define CELLS_COUNT_SST (TERM_MAX_HEIGHT * (40 + 80) * (TERM_ALT_SCREEN ? 2 : 1))
#define CELLS_MEM_SST_SIZE (4 * CELLS_COUNT_SST)
#define CELLS_COLORS_SST_SIZE (6 * CELLS_COUNT_SST)
#define CELLS_PALETTE_SST_SIZE (2 * 258)
#define CELLS_SST_SIZE (3 + TERM_SST_SIZE + 2 * CELLS_ONE_SST_SIZE + 2 + CELLS_MEM_SST_SIZE + \
                        CELLS_COLORS_SST_SIZE + CELLS_PALETTE_SST_SIZE)
void cells_sst_save(sst_cursor_t *c, unsigned flags);
bool cells_sst_load(sst_cursor_t *c, unsigned flags);

#define CELLS_DRIVER DRIVER(                                    \
    INIT(cells_init)                                            \
    TASK_FATFS(cells_task)                                      \
    SST(.id = "CELL",                                           \
        .version = 13,                                          \
        .size = CELLS_SST_SIZE,                                 \
        .save = cells_sst_save,                                 \
        .load = cells_sst_load))

/* ---- what the terminal asks of the screen ------------------------------
 *
 * term.c and grid.c call these. A machine that only follows the terminal's
 * state, with no screen, defines them itself: replies its own way, and the
 * rest as nothing.
 *
 * The terminal has already changed its own state when it calls one, so the
 * cursor, the margins and the line sizes are the new ones. Colors come from
 * the pen, and an erased cell takes the pen's colors, not reversed. The cells follow t->alt_active for which screen is showing. All
 * but the colors and the replies are for one grid; the cells find their own
 * from it. A program's own size (term_add_grid) is a grid with no cells,
 * which these ignore. */

// An answer to a query.
void cells_reply(const term_t *t, const char *s, size_t n);

/* The terminal was sent a BEL that is a bell, and not the end of a string:
 * the console's bell rings, on the machine that has it. */
void cells_bell(const term_t *t);

// The grid the cursor reports come from: the one showing.
term_grid_t *cells_shown(const term_t *t);

// Whether row y of a grid's screen has anything on it: a character other
// than a space, a background that is not the default one, or a line through
// a cell. A machine with no cells answers from what it was told as the row
// was written, and yes where that does not say. No for a grid with no
// cells.
bool cells_row_used(term_t *t, term_grid_t *g, uint8_t y);

// Row y is about to be written or have the cursor on it: a lazy erase still
// owed to it is done now.
void cells_touch(term_t *t, term_grid_t *g, uint8_t y);

// n copies of the glyph at row y from column x, in the pen and the active
// character set. n never runs past the row's columns, and the row is the
// cursor's or one the caller has touched.
void cells_glyphs(term_t *t, term_grid_t *g, uint8_t y, uint8_t x, uint8_t byte, unsigned n);

// Every cell of row y becomes the glyph, in the pen and the active character
// set, when the row is next touched or the task reaches it. The cursor's
// row is never left owing one.
void cells_fill_row(term_t *t, term_grid_t *g, uint8_t y, uint8_t byte);

// Erase columns [from, to) of row y, which is the cursor's or one the
// caller has touched. A selective erase leaves the protected cells.
void cells_erase(term_t *t, term_grid_t *g, uint8_t y, unsigned from, unsigned to,
                 bool selective);

// Insert n erased cells at column x of row y, the cells up to column end
// moving right and off it; or delete n there, the cells up to end moving
// left and erased cells coming in before it. n is at most end - x.
void cells_insert(term_t *t, term_grid_t *g, uint8_t y, uint8_t x, unsigned end, unsigned n);
void cells_delete(term_t *t, term_grid_t *g, uint8_t y, uint8_t x, unsigned end, unsigned n);

// SL and SR: columns [from, end) of rows [top, bot] move n columns left or
// right, erased cells coming in at the other side.
void cells_shift(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, unsigned from,
                 unsigned end, unsigned n, bool left);

// n cells of row sy from column sx are copied to row dy from column dx,
// colors, attributes and all. The two runs may overlap.
void cells_copy(term_t *t, term_grid_t *g, uint8_t sy, unsigned sx, uint8_t dy, unsigned dx,
                unsigned n);

// DECCARA and DECRARA: columns [from, to) of row y have these attributes
// set, cleared, then reversed, whatever else the cells hold, when the row
// is next touched or the task reaches it.
#define CELLS_MARK_BOLD 1u
#define CELLS_MARK_UNDERLINE 2u
#define CELLS_MARK_BLINK 4u
#define CELLS_MARK_REVERSE 8u
#define CELLS_MARK_ALL 15u
void cells_attrs(term_t *t, term_grid_t *g, uint8_t y, unsigned from, unsigned to,
                 unsigned set, unsigned clear, unsigned toggle);

// Lazy-erase rows [from, to), selectively or not.
void cells_erase_rows(term_t *t, term_grid_t *g, uint8_t from, uint8_t to,
                      bool selective);

// DECALN: every row lazy-erased to E's in the default colors.
void cells_align(term_t *t, term_grid_t *g);

// Rows [top, bot] move n rows up or down, erased rows coming in.
void cells_scroll(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, uint8_t n, bool up);

// The same for only columns [from, end) of those rows, which are not the
// whole width: a scroll between left and right margins.
void cells_scroll_cols(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, unsigned from,
                       unsigned end, uint8_t n, bool up);

// The whole screen moves a row up, the top row joining the scrollback, or a
// row down, the newest row of the scrollback lost.
void cells_index(term_t *t, term_grid_t *g, bool up);

// ED 3: the scrollback is forgotten.
void cells_forget(term_t *t, term_grid_t *g);

// The screen showing lazy-erased and its scrollback forgotten.
void cells_clear(term_t *t, term_grid_t *g);

// The height went one row up or down. With top, the cursor was on the bottom
// row and the screen moved instead of the cursor: a grow brings a row back
// from the scrollback at the top, a shrink sends the top row to it.
void cells_height(term_grid_t *g, bool grow, bool top);

// RIS and the system's soft reset: the blinks start again and the alternate
// screen is erased.
void cells_reset(term_t *t, term_grid_t *g);

// RIS and the system's soft reset: the palette and the default and cursor
// colors start again. Once a terminal, before the grids reset.
void cells_colors_reset(term_t *t);

// The OSC colors. which is a palette entry 0-255, or one of these.
#define CELLS_COLOR_FG 256
#define CELLS_COLOR_BG 257
#define CELLS_COLOR_CURSOR 258
void cells_color(term_t *t, unsigned which, uint16_t color);
void cells_color_reset(term_t *t, unsigned which);
// The color as it is now, for a program that asks (OSC 4, 10, 11 and 12
// with a "?").
uint16_t cells_color_get(const term_t *t, unsigned which);

#endif /* _CORE_TERM_CELLS_H_ */
