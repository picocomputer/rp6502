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

/* The terminal and the cells of both sizes as at power-up: every cell blank,
 * new grids at TERM_STD_HEIGHT, and a RIS. It is the RESET hook, because a
 * load that fails leaves any value in all of them. The renderer's RESET,
 * which runs first, leaves setting the heights again to its next task. */
void cells_restart(void);

/* The console's terminal. On a machine that runs a program and draws its
 * screen in one, it is draw.c's; the RIA has its own, which follows the
 * VGA's (ria/term/cells.c). */
extern term_t cells_term;

// RIS, and the system's soft reset, which also sets the 40-column terminal
// back to TERM_STD_HEIGHT: only a mode 0 on a 320-wide canvas sets its
// height, so after either chip restarts alone the two could differ.
void cells_RIS(void);
void cells_RIS_no_clear(void);

/* Writes mode 0's words for the size showing. cells_out_chars calls it
 * after each batch of the console's bytes, and cells_task after the blinks,
 * a reset or a new height. Something else that changes what shows calls it
 * too: a savestate load that puts the canvas back, or a test that feeds
 * term_write directly. */
void cells_publish(void);

// The renderer reports its geometry: rows of the 40- or 80-column terminal.
void cells_set_height(uint8_t width, uint8_t height);

/* The chunk holds the terminal (term.h) and both sizes in full: each one's
 * grid, the layout of its screens' cells (the scroll remap and the fills
 * its rows are owed) and its blinks, then the cursor's color. The cells,
 * their colors and the palette are MODE0_DRIVER's (mode0.h), which follows
 * this one in the driver list. */
extern const sst_table_t cells_sst;

#define CELLS_DRIVER DRIVER(                                    \
    INIT(cells_init)                                            \
    TASK_FATFS(cells_task)                                      \
    RESET(cells_restart)                                        \
    SST("CELL", cells))

/* ---- what the terminal asks of the console -----------------------------
 *
 * term.c and grid.c call these. A machine that only follows the terminal's
 * state, with no screen, defines them itself: replies its own way, and the
 * rest as it needs. Each grid changes its screens' rows and cells through
 * the cell store (screen.h). */

// An answer to a query.
void cells_reply(const term_t *t, const char *s, size_t n);

/* The terminal was sent a BEL that is a bell, and not the end of a string:
 * the console's bell rings, on the machine that has it. */
void cells_bell(const term_t *t);

// The grid the cursor reports come from: the one showing.
term_grid_t *cells_shown(const term_t *t);

// Under DECSCLM, rows [top, bot] just scrolled a row up or down by an index
// or a reverse index, and the screen shows them move there a scanline a
// frame from the next frame, over TERM_SMOOTH_FRAMES. ghost is the row of
// memory the row that went is in, which screen_scroll or screen_index kept
// with its line size; it shows until it is gone. Nothing writes the
// ghost until the next scroll, index, clear or change of height of the
// grid, each of which ends the smooth scroll, and the terminal holds a byte
// that would scroll the rows until they land (term_holds). A scroll still
// moving finishes at once. A machine that draws records it for the size
// showing; one that only follows holds the console's bytes for it.
void cells_smooth(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, bool up, uint8_t ghost);

// A smooth scroll of g is still moving.
bool cells_smooth_busy(const term_grid_t *g);

// RIS and the system's soft reset: the palette and the default and cursor
// colors start again, and on a machine that draws the screen the blinks
// start again and a smooth scroll stops. Once a terminal, before the grids
// reset.
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
