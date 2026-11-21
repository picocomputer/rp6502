/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_CONSOLE_TERM_H_
#define _CORE_TERM_CONSOLE_TERM_H_

/* The console's terminal, the same on every machine: the terminal (term.h)
 * followed at 40 and 80 columns, the two sizes of the VGA's console, and
 * what the terminal asks of the console: the size showing, smooth scrolls
 * and the palette (color.h). A machine that draws the console (draw.c)
 * hands it mode 0's cells, and mode 0 draws from the palette. The RIA's
 * terminal follows the VGA's: it is fed the bytes the VGA's terminal gets
 * as they are sent, ria/sys/vga.c replays the VGA's resets and size changes
 * in the same order, and it hands in no cells.
 *
 * The cell store takes more memory than anything else here, and two things
 * size it: TERM_ALT_SCREEN, and the machine's TERM_MAX_HEIGHT — 32 rows only
 * where the device's 512-line SXGA console exists, 30 everywhere else. */

#include "core/term/color.h"
#include "core/term/grid.h"
#include "core/term/screen.h"
#include "core/term/term.h"
#include "core/vga/mode/mode0.h"
#include <stdbool.h>
#include <stdint.h>

// The console's terminal at one size: its grid, and the storage of its
// screens, the primary's then the alternate's: the line attributes of their
// rows of memory and, where the screens keep cells, their rings and cells.
// A build without TERM_ALT_SCREEN gives the alternate screen no cells.
typedef struct
{
    term_grid_t grid;
#if TERM_CELLS
    uint8_t lines[2 * TERM_MEM_ROWS];
    uint8_t row_idx[2 * TERM_MAX_HEIGHT];
    screen_cells_t bufs[2];
#else
    uint8_t lines[2 * TERM_MAX_HEIGHT];
#endif
} console_size_t;

extern term_t console_term;
extern console_size_t console_40;
extern console_size_t console_80;

/* The terminal and both sizes as at power-up: new grids at TERM_STD_HEIGHT,
 * and a new terminal on them, whose RIS lays out every screen and sets the
 * palette. cells are MODE0_CELLS cells for the screens, each size's primary
 * then its alternate, the 40's first, and every one of them is a blank in
 * the default colors at once; NULL where the screens keep no cells. A
 * program's own grid is dropped. */
void console_init(mode0_cell_t *cells);

// RIS, and the system's soft reset, which also sets the 40-column terminal
// back to TERM_STD_HEIGHT: only a mode 0 on a 320-wide canvas sets its
// height, so after either chip restarts alone the two could differ.
void console_RIS(void);
void console_RIS_no_clear(void);

// The renderer reports its geometry: rows of the 40- or 80-column terminal.
void console_set_height(uint8_t width, uint8_t height);

// The grid the cursor reports come from, and the one drawn: the 40 on a
// 320-wide canvas of a connected VGA, otherwise the 80, which is the
// console with no VGA too.
term_grid_t *console_shown(const term_t *t);

/* ---- smooth scroll ------------------------------------------------------
 *
 * Under DECSCLM, rows [top, bot] of the grid showing just scrolled a row up
 * or down by an index or a reverse index, and the VGA shows them move there
 * a scanline a frame from the next frame, over TERM_SMOOTH_FRAMES. ghost is
 * the row of memory the row that went is in, which screen_scroll or
 * screen_index kept with its line size; it shows until it is gone. Nothing
 * writes the ghost until the next scroll, index, clear or change of height
 * of the grid, each of which ends the smooth scroll, as a reset does, and
 * the terminal holds a byte that would scroll the rows until they land
 * (term_holds). A scroll still moving finishes at once. The frames are the
 * VGA's, vga_frame_count. With no VGA the console is a serial port to a
 * terminal of unknown kind, and nothing moves or is held. A machine that
 * draws the console draws the scroll from this record (draw.c). One whose
 * screens keep no cells (TERM_CELLS 0) only holds the bytes, and its record
 * is the grid and the frame alone. */
typedef struct
{
    const term_grid_t *grid; // the grid moving, or NULL for none
    uint32_t frame;          // the first frame that shows the rows on their way
#if TERM_CELLS
    uint32_t cancels; // the store's count of the grid's cancels as it started
    uint8_t screen;   // which of its screens: 0 the primary, 1 the alternate
    uint8_t top, bot;
    bool up;
    uint8_t ghost;
#endif
} console_smooth_t;
extern console_smooth_t console_smoothing;

void console_smooth(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, bool up,
                    uint8_t ghost);

// The grid a smooth scroll is still moving, or NULL for none. One that has
// landed, or that the store ended, is put away first, as it is when the VGA
// is gone, so the record holds only a scroll still moving. It runs on the
// RIA's drain path from RAM.
const term_grid_t *console_smooth_grid(void);

// A smooth scroll of g, one of the console's grids, is still moving
// (console_smooth_grid).
static inline HOST_IN_RAM_FUNC bool console_smooth_busy(const term_grid_t *g)
{
    return console_smooth_grid() == g;
}

// The store's count of the cancels of g's screens (screen.h), for a record
// that starts again from it.
uint32_t console_cancels(const term_grid_t *g);

/* ---- resets and colors -------------------------------------------------
 *
 * A palette entry or default color that changes reaches every cell drawn
 * with it, as on xterm and Linux, because those cells hold the entry and
 * not the color. */

// A color of the palette changed since draw.c last followed it: mode 0's
// faint twins are made from the palette (draw.c), which clears this.
extern bool console_palette_changed;

// RIS and the system's soft reset: the colors start again
// (console_colors_reset), a smooth scroll stops, and on a machine that
// draws the console the blinks start again (draw_reset). Once a terminal,
// before the grids reset.
void console_term_reset(term_t *t);

// The OSC colors. which is an entry of console_palette: 0-255, or one of
// CONSOLE_COLOR_FG, _BG and _CURSOR (color.h).
void console_color(term_t *t, unsigned which, uint16_t color);
void console_color_reset(term_t *t, unsigned which);
// The palette and the default and cursor colors as at power-up, and
// nothing else.
void console_colors_reset(term_t *t);
// The color as it is now, for a program that asks (OSC 4, 10, 11 and 12
// with a "?").
uint16_t console_color_get(const term_t *t, unsigned which);

#endif /* _CORE_TERM_CONSOLE_TERM_H_ */
