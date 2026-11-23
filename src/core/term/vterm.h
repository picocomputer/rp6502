/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_VTERM_H_
#define _CORE_TERM_VTERM_H_

/* The console's terminal, the same on every machine: the terminal (term.h)
 * followed at 40 and 80 columns, the two sizes of the VGA's console, and
 * what the terminal asks of the console: the size showing, smooth scrolls
 * and the palette (color.h). A machine that draws the console (draw.c)
 * hands it mode 0's cells, and mode 0 draws from the palette. The RIA's
 * terminal follows the VGA's: it is fed the bytes the VGA's terminal gets
 * as they are sent, ria/sys/vga.c replays the VGA's resets, canvases and
 * mode 0 programs in the same order, and it hands in text cells
 * (TERM_CELL_TEXT), which keep the text and draw nothing.
 *
 * vterm is the VGA's terminal, kept on every chip. term_ is the engine it
 * runs (term.h), and console_ is CON: (core/com/console.h).
 *
 * The cell store takes more memory than anything else here, and two things
 * size it: TERM_ALT_SCREEN, and the machine's TERM_MAX_HEIGHT — 32 rows only
 * where the device's 512-line SXGA console exists, 30 everywhere else. */

#include "core/term/color.h"
#include "core/term/grid.h"
#include "core/term/screen.h"
#include "core/term/term.h"
#include <stdbool.h>
#include <stdint.h>

// The console's terminal at one size: its grid, and the storage of its
// screens, the primary's then the alternate's: the line attributes of their
// rows of memory, their rings and their cells. A build without
// TERM_ALT_SCREEN gives the alternate screen no cells.
typedef struct
{
    term_grid_t grid;
    uint8_t lines[2 * TERM_MEM_ROWS];
    uint8_t row_idx[2 * TERM_MAX_HEIGHT];
    screen_cells_t bufs[2];
} vterm_size_t;

extern term_t vterm;
extern vterm_size_t vterm_40;
extern vterm_size_t vterm_80;

/* The terminal and both sizes as at power-up: new grids at TERM_STD_HEIGHT,
 * and a new terminal on them, whose RIS lays out every screen and sets the
 * palette. cells are VTERM_CELLS cells of the machine's kind (cell.h) for
 * the screens, each size's primary then its alternate, the 40's first, and
 * every one of them is a blank in the default colors at once. A program's
 * own grid is dropped. */
#define VTERM_SCREENS (TERM_ALT_SCREEN ? 2 : 1)
#define VTERM_CELLS ((40 + 80) * TERM_MEM_ROWS * VTERM_SCREENS)
void vterm_init(cell_t *cells);

/* Mode 0's cells are drawn (draw.c), and text cells are not: a machine
 * with text cells keeps nothing for drawing. */
#define VTERM_DRAWN (!TERM_CELL_TEXT)

// RIS, and the system's soft reset, which also sets the 40-column terminal
// back to TERM_STD_HEIGHT: only a mode 0 on a 320-wide canvas sets its
// height, so after either chip restarts alone the two could differ. These,
// vterm_place and vterm_unplace first finish the jobs pending
// (term_job_finish), as draw.c does before a byte of the console.
void vterm_RIS(void);
void vterm_RIS_no_clear(void);

/* ---- where mode 0 sits --------------------------------------------------
 *
 * The canvas the console's terminal is shown on, and where mode 0 draws it
 * there, kept the same on every chip: the VGA's as it selects canvases and
 * programs mode 0, and the RIA's as it sends what does. The size showing
 * is cols, the 40 on a 320-wide canvas and otherwise the 80
 * (vterm_get_shown). Mode 0 is placed once it is programmed on the canvas
 * selected, the console's own program on the console canvas included, and
 * stays placed until a canvas is selected again, whatever is programmed
 * over it later; only the console canvas selected from itself keeps it.
 * begin, rows and row_px then say where its rows are; they are 0 while it
 * is not placed. A call that finds the record as it would leave it changes
 * nothing in it, so the console canvas selected again from the console
 * canvas, or mode 0 programmed again where it is, keeps the record as it
 * was. */
typedef struct
{
    int16_t begin;  // the scanline of the canvas its first row begins on
    uint8_t cols;   // 40 or 80: the size showing
    uint8_t rows;   // its rows, the height of the terminal of that size
    uint8_t row_px; // scanlines a row: 8 at 40 columns, 16 at 80
    bool console;   // the console canvas is selected
    bool placed;    // mode 0 is programmed on the canvas
} vterm_placement_t;
extern vterm_placement_t vterm_placement;

// A canvas is selected, whose width shows the size of cols. Nothing is
// placed on it yet, unless it is the console canvas selected again from
// the console canvas, which keeps the console's mode 0 where it is.
void vterm_canvas(uint8_t cols, bool is_console);

// Mode 0 is programmed on the canvas: the terminal of cols has rows, the
// first beginning on scanline begin, row_px scanlines a row. The height is
// set through term_set_height every time, as a mode 0 program always has.
void vterm_place(uint8_t cols, uint8_t rows, int16_t begin, uint8_t row_px);

// The RIA with no VGA: nothing is placed, and the console is a terminal on
// the UART, taken to be 80x24.
void vterm_unplace(void);

/* ---- the selection ------------------------------------------------------
 *
 * The console's one selection, which mode 0 highlights (draw.c), kept the
 * same on every chip: vterm_selection, which screen.h declares, since the
 * store drops it. Mode 0 placed somewhere new, the console canvas coming
 * back from another canvas (vterm_canvas, vterm_place), vterm_unplace and
 * vterm_RIS clear it. Mode 0 programmed again where it is, and the console
 * canvas selected from itself, keep it, and so does vterm_RIS_no_clear on
 * a primary screen. That reset erases each grid's alternate screen, which
 * drops one there, and sets the 40-column terminal's height back, which
 * drops one on that terminal at any other height. */

// The user selects, on screen (0 the primary, 1 the alternate) of g, one of
// the console's grids, from row of memory start_row, column start_col up
// to row of memory end_row, column end_col, that cell not taken. The
// columns are each row's own, half the width on a row of double width. Its
// start is a row of the height no lower on the screen than its end, and
// its columns are no more than their rows have; a start that is its end,
// or ends that are not so, select nothing. A change of the user's, for the
// RIA to send (vterm_selection.unsent), as is vterm_unselect.
void vterm_select(const term_grid_t *g, uint8_t screen, uint8_t start_row, uint8_t start_col,
                  uint8_t end_row, uint8_t end_col);

// The user ends the selection.
void vterm_unselect(void);

/* The selection as two words of the VGA's control channel, its start and
 * its end, each the column in bits 6:0, the row of memory in bits 12:7, the
 * screen in bit 13 and the grid in bit 14, 1 for the 80-column one. None,
 * or one whose highlight was dropped, is two words the same. The RIA sends
 * them (ria/sys/vga.c), and a save state keeps them (draw.c). */
void vterm_select_words(uint16_t *start, uint16_t *end);

// The selection becomes the one of the words: the VGA's when the RIA sends
// them, at once, whatever job is pending, since the RIA judged that job as
// it started, before the change, as the VGA did; and a save state's. It
// counts no change of the user's, which only the RIA sends. False when they
// select no rows of the height of a screen this build has.
bool vterm_select_apply(uint16_t start, uint16_t end);

/* The code page the console's glyphs show, as font_code_page_of gives it:
 * the fonts' own page on a machine that draws them (font.c), and on the RIA
 * the page the VGA's fonts hold, followed while the VGA is connected. */
uint16_t vterm_get_code_page(void);

// Choosing a different page resets the terminal, because a screen of
// glyphs from one page cannot be read against another. True when the page
// changed, and the glyphs are to be written.
bool vterm_set_code_page(uint16_t cp);

// The page without the terminal reset, for a savestate, which has already
// put the terminal's cells back.
void vterm_load_code_page(uint16_t cp);

// The grid the cursor reports come from, and the one drawn: the 40 on a
// 320-wide canvas of a connected VGA (vterm_placement), otherwise the 80,
// which is the console with no VGA too.
term_grid_t *vterm_get_shown(const term_t *t);

/* ---- smooth scroll ------------------------------------------------------
 *
 * Under DECSCLM, rows [top, bot] of the grid showing just scrolled a row up
 * or down by an index or a reverse index, and the VGA shows them move there
 * a scanline a frame from the next frame, over TERM_SMOOTH_FRAMES. ghost is
 * the row of memory the row that went is in, which screen_scroll or
 * screen_index kept with its line size; it shows until it is gone. A REP
 * whose glyphs filled it before they scrolled it out writes the glyph there
 * too: screen_rep at once in the spare row kept aside from the ring, the
 * REP's job as one of its rows in the newest row above row 0. Nothing else
 * writes it until the next scroll, index, clear or change of height of the
 * grid, each of which ends the smooth scroll, as a reset does, and the
 * terminal holds a byte that would scroll the rows until they land
 * (term_holds). A scroll still moving finishes at once. The frames are the
 * VGA's, vga_frame_count. With no VGA the console is a serial port to a
 * terminal of unknown kind, and nothing moves or is held. A machine that
 * draws the console draws the scroll from this record (draw.c). One that
 * draws nothing (VTERM_DRAWN 0) only holds the bytes, and its record is
 * the grid, the frame and the cancels alone. */
typedef struct
{
    const term_grid_t *grid; // the grid moving, or NULL for none
    uint32_t frame;          // the first frame that shows the rows on their way
    uint32_t cancels;        // the grid's count of cancels as it started (grid.h)
#if VTERM_DRAWN
    uint8_t screen; // which of its screens: 0 the primary, 1 the alternate
    uint8_t top, bot;
    bool up;
    uint8_t ghost;
#endif
} vterm_smooth_t;
extern vterm_smooth_t vterm_smoothing;

void vterm_smooth(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, bool up, uint8_t ghost);

// The grid a smooth scroll is still moving, or NULL for none. One that has
// landed, or that its grid ended, is put away first, as it is when the VGA
// is gone, so the record holds only a scroll still moving. It runs on the
// RIA's drain path from RAM.
const term_grid_t *vterm_smooth_grid(void);

// A smooth scroll of g, one of the console's grids, is still moving
// (vterm_smooth_grid).
static inline HOST_IN_RAM_FUNC bool vterm_smooth_busy(const term_grid_t *g)
{
    return vterm_smooth_grid() == g;
}

/* ---- resets and colors -------------------------------------------------
 *
 * A palette entry or default color that changes reaches every cell drawn
 * with it, as on xterm and Linux, because those cells hold the entry and
 * not the color. */

// A color of the palette or a special color changed since draw.c last
// followed them: mode 0's faint twins are made from them (draw.c), which
// clears this.
extern bool vterm_palette_changed;

// RIS and the system's soft reset: the colors start again
// (vterm_reset_colors), a smooth scroll stops, and on a machine that
// draws the console the blinks start again (draw_reset). Once a terminal,
// before the grids reset.
void vterm_reset(term_t *t);

// The OSC colors. which is an entry of vterm_palette, 0-255, or a special
// color: VTERM_COLOR_FG, _BG or _CURSOR (color.h). A cursor color that is
// the default foreground follows a change to the default foreground, as
// on xterm.
void vterm_set_color(term_t *t, unsigned which, uint16_t color);
void vterm_reset_color(term_t *t, unsigned which);
// The palette and the default and cursor colors as at power-up, and
// nothing else.
void vterm_reset_colors(term_t *t);
// The color as it is now, for a program that asks (OSC 4, 10, 11 and 12
// with a "?").
uint16_t vterm_get_color(const term_t *t, unsigned which);

#endif /* _CORE_TERM_VTERM_H_ */
