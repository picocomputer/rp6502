/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/cells.h"
#include "core/term/color.h"
#include "core/term/console_term.h"
#include "core/term/grid.h"
#include "core/term/screen.h"
#include "core/term/term.h"
#include "core/vga/vga.h"
#include "machine.h"

term_t console_term;
console_size_t console_40;
console_size_t console_80;
console_smooth_t console_smoothing;
uint16_t console_palette[CONSOLE_COLORS];
bool console_palette_changed;

/* draw.c's CELL chunk saves the terminal, both sizes, the smooth scroll and
 * the palette. */
#define CONSOLE_SST(X)                                                 \
    X(SKIP, console_term, "draw.c's CELL chunk saves it")              \
    X(SKIP, console_40, "draw.c's CELL chunk saves it")                \
    X(SKIP, console_80, "draw.c's CELL chunk saves it")                \
    X(SKIP, console_smoothing, "draw.c's CELL chunk saves it")         \
    X(SKIP, console_palette, "draw.c's CELL chunk saves it")           \
    X(SKIP, console_palette_changed, "draw.c follows every color "     \
                                     "after a load")
SST_TABLE(console_term, CONSOLE_SST)

#define CONSOLE_FG_COLOR_INDEX 7
#define CONSOLE_BG_COLOR_INDEX 0

/* New grids at both sizes, their screens laid out in this file's line sizes
 * and rings and given their cells: each size's primary, then its alternate.
 * Without an alternate store, the alternate screen has no cells, and the
 * terminal never switches to it. */
#if TERM_CELLS
void console_init(cell_t *cells)
#else
void console_init(mode0_cell_t *cells)
#endif
{
    console_size_t *const all[] = {&console_40, &console_80};
#if TERM_CELLS
    screen_blank_cells(cells, CONSOLE_CELLS);
    for (int n = 0; n < 2; n++)
    {
        term_grid_init(&all[n]->grid, n ? 80 : 40, all[n]->lines, all[n]->row_idx,
                       TERM_MAX_HEIGHT);
        for (int b = 0; b < CONSOLE_SCREENS; b++)
        {
            all[n]->bufs[b].mem = cells;
            all[n]->bufs[b].width = n ? 80 : 40;
            cells += all[n]->bufs[b].width * TERM_MEM_ROWS;
            screen_set_cells(&all[n]->grid.screen[b], &all[n]->bufs[b]);
        }
    }
#else
    (void)cells;
    for (int n = 0; n < 2; n++)
        term_grid_init(&all[n]->grid, n ? 80 : 40, all[n]->lines, NULL, TERM_MAX_HEIGHT);
#endif
    term_grid_t *const grids[] = {&console_40.grid, &console_80.grid};
    term_init(&console_term, grids, 2);
}

void console_RIS(void)
{
    term_RIS(&console_term);
}

void console_RIS_no_clear(void)
{
    term_set_height(&console_term, &console_40.grid, TERM_STD_HEIGHT);
    term_RIS_no_clear(&console_term);
}

void console_set_height(uint8_t width, uint8_t height)
{
    term_set_height(&console_term, width == 40 ? &console_40.grid : &console_80.grid, height);
}

term_grid_t *console_shown(const term_t *t)
{
    (void)t;
    return vga_connected() && vga_canvas_width() == 320 ? &console_40.grid : &console_80.grid;
}

/* ---- smooth scroll ----------------------------------------------------- */

// Only the size showing moves.
void console_smooth(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, bool up,
                    uint8_t ghost)
{
    if (!vga_connected() || g != console_shown(t))
        return;
    console_smoothing = (console_smooth_t){
        .grid = g,
        .frame = (uint32_t)vga_frame_count() + 1,
#if TERM_CELLS
        .cancels = g->cancels,
        .screen = (uint8_t)(g->shown - g->screen),
        .top = top,
        .bot = bot,
        .up = up,
        .ghost = ghost,
#endif
    };
#if !TERM_CELLS
    (void)top, (void)bot, (void)up, (void)ghost;
#endif
}

/* The rows have landed by the frame before the last of TERM_SMOOTH_FRAMES,
 * which shows them still. The record is put away then, so the frame count
 * wrapping round long after can never show it again. */
const term_grid_t *HOST_IN_RAM_FUNC console_smooth_grid(void)
{
    console_smooth_t *sm = &console_smoothing;
    if (sm->grid &&
        (!vga_connected() ||
#if TERM_CELLS
         sm->cancels != sm->grid->cancels ||
#endif
         (int32_t)((uint32_t)vga_frame_count() - (sm->frame + TERM_SMOOTH_FRAMES - 1)) >= 0))
        sm->grid = NULL;
    return sm->grid;
}

/* ---- resets and colors ------------------------------------------------- */

void console_color(term_t *t, unsigned which, uint16_t color)
{
    (void)t;
    console_palette[which] = color;
    console_palette_changed = true;
}

void console_color_reset(term_t *t, unsigned which)
{
    console_color(t, which, color_256[which < 256                   ? which
                                      : which == CONSOLE_COLOR_BG ? CONSOLE_BG_COLOR_INDEX
                                                                  : CONSOLE_FG_COLOR_INDEX]);
}

uint16_t console_color_get(const term_t *t, unsigned which)
{
    (void)t;
    return console_palette[which];
}

void console_colors_reset(term_t *t)
{
    for (unsigned i = 0; i < CONSOLE_COLORS; i++)
        console_color_reset(t, i);
}

void console_term_reset(term_t *t)
{
    console_smoothing.grid = NULL;
    console_colors_reset(t);
    draw_reset();
}
