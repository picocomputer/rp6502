/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_CELLS_H_
#define _CORE_TERM_CELLS_H_

/* The console's screen on a machine that draws it: the terminal (term.h)
 * followed at 40 and 80 columns, fed every byte of the console, and the
 * cells, blinks and save state of both sizes. A renderer draws the one showing, read through
 * cells_view and cells_view_row. mode0.c is the scanvideo renderer; hosts
 * with their own scanout hardware bring their own.
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

typedef struct
{
    uint8_t font_code;
    uint8_t attributes; // TERM_ATTR_
    uint16_t fg_color;
    uint16_t bg_color;
    uint16_t ul_color; // resolved at cell-write: SGR 58 color, or fg if not set
} term_data_t;

_Static_assert(sizeof(term_data_t) == 8, "term_data_t size lock for cell-memory budget");

// The visible terminal's cursor and blink state, one call a scanline or a
// frame as the renderer prefers.
typedef struct
{
    uint8_t width;
    uint8_t height;
    uint8_t cursor_x, cursor_y, cursor_style;
    bool cursor_enabled, cursor_lit;
    uint16_t cursor_color;
    uint8_t blink_phase;
    bool reverse; // DECSCNM: every cell's colors swapped
} cells_view_t;
void cells_view(cells_view_t *out);

// The visible terminal's logical row y, resolved through the scroll remap, so
// the caller sees the right row after a region scroll or an alt-screen swap.
const term_data_t *cells_view_row(uint8_t y);

// The size of the visible terminal's logical row y, a TERM_LINE_ value.
uint8_t cells_view_row_size(uint8_t y);

// The renderer reports its geometry: rows of the 40- or 80-column terminal.
void cells_set_height(uint8_t width, uint8_t height);

/* The chunk holds the terminal (term.h) and both sizes in full: each one's
 * grid, the layout of its screens' cells (the scroll remap and the pending
 * lazy erases) and its blinks, then the colors and the cells. It also holds
 * the runtime palette, because OSC 4 can change an entry and nothing else
 * records what it changed to.
 *
 * The cell arrays' own addresses are never written, because they are
 * function-scope statics in cells_init, which runs long before any load.
 *
 * The chunk starts with the terminal dimensions and a load rejects a chunk
 * whose dimensions differ, because a blob made at a taller height would put
 * its rows in the wrong places. */
#define CELLS_BUF_SST_SIZE (7 * TERM_MAX_HEIGHT + 3)
#define CELLS_ONE_SST_SIZE (TERM_GRID_SST_SIZE(TERM_MAX_HEIGHT) + 2 * CELLS_BUF_SST_SIZE + 18)
#define CELLS_MEM_SST_SIZE (8 * TERM_MAX_HEIGHT * (40 + 80) * (TERM_ALT_SCREEN ? 2 : 1))
#define CELLS_SST_SIZE (3 + TERM_SST_SIZE + 2 * CELLS_ONE_SST_SIZE + 6 + CELLS_MEM_SST_SIZE + 512)
void cells_sst_save(sst_cursor_t *c, unsigned flags);
bool cells_sst_load(sst_cursor_t *c, unsigned flags);

#define CELLS_DRIVER DRIVER(                                    \
    INIT(cells_init)                                            \
    TASK_FATFS(cells_task)                                      \
    SST(CELL, 1, CELLS_SST_SIZE, cells_sst_save, cells_sst_load))

/* ---- what the terminal asks of the screen ------------------------------
 *
 * term.c and grid.c call these. A machine that only follows the terminal's
 * state, with no screen, defines them itself: replies its own way, and the
 * rest as nothing.
 *
 * The terminal has already changed its own state when it calls one, so the
 * cursor, the margins and the line sizes are the new ones. Colors come from
 * the pen, resolved here, and an erased cell takes the pen's colors, not
 * reversed. The cells follow t->alt_active for which screen is showing. All
 * but the colors and the replies are for one grid; the cells find their own
 * from it. A program's own size (term_add_grid) is a grid with no cells,
 * which these ignore. */

// An answer to a query.
void cells_reply(const term_t *t, const char *s, size_t n);

// The grid the cursor reports come from: the one showing.
term_grid_t *cells_shown(const term_t *t);


// Row y is about to be written or have the cursor on it: a lazy erase still
// owed to it is done now.
void cells_touch(term_t *t, term_grid_t *g, uint8_t y);

// n copies of the glyph at row y from column x, in the pen and the active
// character set. n never runs past the row's columns.
void cells_glyphs(term_t *t, term_grid_t *g, uint8_t y, uint8_t x, uint8_t byte, unsigned n);

// Erase columns [from, to) of row y.
void cells_erase(term_t *t, term_grid_t *g, uint8_t y, unsigned from, unsigned to);

// Insert n erased cells at column x of row y, the rest of the row moving
// right and off the end; or delete n there, the rest moving left and erased
// cells coming in at the end.
void cells_insert(term_t *t, term_grid_t *g, uint8_t y, uint8_t x, unsigned n);
void cells_delete(term_t *t, term_grid_t *g, uint8_t y, uint8_t x, unsigned n);

// SL and SR: rows [top, bot] move n columns left or right, erased cells
// coming in at the other side.
void cells_shift(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, unsigned n, bool left);

// Lazy-erase rows [from, to).
void cells_erase_rows(term_t *t, term_grid_t *g, uint8_t from, uint8_t to);

// DECALN: every row lazy-erased to E's in the default colors.
void cells_align(term_t *t, term_grid_t *g);

// Rows [top, bot] move n rows up or down, erased rows coming in.
void cells_scroll(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, uint8_t n, bool up);

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
void cells_height(term_t *t, term_grid_t *g, bool grow, bool top);

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

#endif /* _CORE_TERM_CELLS_H_ */
