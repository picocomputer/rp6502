/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/draw.h"
#include "core/term/color.h"
#include "core/term/grid.h"
#include "core/term/screen.h"
#include "core/term/term.h"
#include "core/term/vterm.h"
#include "core/vga/vga.h"
#include "machine.h"

term_t vterm;
vterm_size_t vterm_40;
vterm_size_t vterm_80;
vterm_smooth_t vterm_smoothing;
uint16_t vterm_palette[VTERM_PALETTE];
uint16_t vterm_specials[VTERM_SPECIALS];
bool vterm_palette_changed;

/* draw.c's CELL chunk saves the terminal, both sizes, the smooth scroll,
 * the palette and the special colors. */
#define VTERM_SST(X)                                             \
    X(SKIP, vterm, "draw.c's CELL chunk saves it")               \
    X(SKIP, vterm_40, "draw.c's CELL chunk saves it")            \
    X(SKIP, vterm_80, "draw.c's CELL chunk saves it")            \
    X(SKIP, vterm_smoothing, "draw.c's CELL chunk saves it")     \
    X(SKIP, vterm_palette, "draw.c's CELL chunk saves it")       \
    X(SKIP, vterm_specials, "draw.c's CELL chunk saves it")      \
    X(SKIP, vterm_palette_changed, "draw.c follows every color " \
                                   "after a load")
SST_TABLE(vterm, VTERM_SST)

#define VTERM_FG_COLOR_INDEX 7
#define VTERM_BG_COLOR_INDEX 0

/* New grids at both sizes, their screens laid out in this file's line sizes
 * and rings and given their cells: each size's primary, then its alternate.
 * Without an alternate store, the alternate screen has no cells, and the
 * terminal never switches to it. */
void vterm_init(cell_t *cells)
{
    vterm_size_t *const all[] = {&vterm_40, &vterm_80};
    screen_blank_cells(cells, VTERM_CELLS);
    for (int n = 0; n < 2; n++)
    {
        term_grid_init(&all[n]->grid, n ? 80 : 40, all[n]->lines, all[n]->row_idx,
                       TERM_MAX_HEIGHT);
        for (int b = 0; b < VTERM_SCREENS; b++)
        {
            all[n]->bufs[b].mem = cells;
            all[n]->bufs[b].width = n ? 80 : 40;
            cells += all[n]->bufs[b].width * TERM_MEM_ROWS;
            screen_set_cells(&all[n]->grid.screen[b], &all[n]->bufs[b]);
        }
    }
    term_grid_t *const grids[] = {&vterm_40.grid, &vterm_80.grid};
    term_init(&vterm, grids, 2);
}

void vterm_RIS(void)
{
    term_RIS(&vterm);
}

void vterm_RIS_no_clear(void)
{
    term_set_height(&vterm, &vterm_40.grid, TERM_STD_HEIGHT);
    term_RIS_no_clear(&vterm);
}

void vterm_set_height(uint8_t width, uint8_t height)
{
    term_set_height(&vterm, width == 40 ? &vterm_40.grid : &vterm_80.grid, height);
}

term_grid_t *vterm_get_shown(const term_t *t)
{
    (void)t;
    return vga_connected() && vga_canvas_width() == 320 ? &vterm_40.grid : &vterm_80.grid;
}

/* ---- smooth scroll ----------------------------------------------------- */

// Only the size showing moves.
void vterm_smooth(term_t *t, term_grid_t *g, uint8_t top, uint8_t bot, bool up, uint8_t ghost)
{
    if (!vga_connected() || g != vterm_get_shown(t))
        return;
    vterm_smoothing = (vterm_smooth_t){
        .grid = g,
        .frame = (uint32_t)vga_frame_count() + 1,
        .cancels = g->cancels,
#if VTERM_DRAWN
        .screen = (uint8_t)(g->shown - g->screen),
        .top = top,
        .bot = bot,
        .up = up,
        .ghost = ghost,
#endif
    };
#if !VTERM_DRAWN
    (void)top, (void)bot, (void)up, (void)ghost;
#endif
}

/* The rows have landed by the frame before the last of TERM_SMOOTH_FRAMES,
 * which shows them still. The record is put away then, so the frame count
 * wrapping round long after can never show it again. */
const term_grid_t *HOST_IN_RAM_FUNC vterm_smooth_grid(void)
{
    vterm_smooth_t *sm = &vterm_smoothing;
    if (sm->grid &&
        (!vga_connected() || sm->cancels != sm->grid->cancels ||
         (int32_t)((uint32_t)vga_frame_count() - (sm->frame + TERM_SMOOTH_FRAMES - 1)) >= 0))
        sm->grid = NULL;
    return sm->grid;
}

/* ---- resets and colors ------------------------------------------------- */

void vterm_set_color(term_t *t, unsigned which, uint16_t color)
{
    (void)t;
    if (which < VTERM_PALETTE)
        vterm_palette[which] = color;
    else
        vterm_specials[which - VTERM_PALETTE] = color;
    vterm_palette_changed = true;
}

void vterm_reset_color(term_t *t, unsigned which)
{
    vterm_set_color(t, which, color_256[which < VTERM_PALETTE     ? which
                                        : which == VTERM_COLOR_BG ? VTERM_BG_COLOR_INDEX
                                                                  : VTERM_FG_COLOR_INDEX]);
}

uint16_t vterm_get_color(const term_t *t, unsigned which)
{
    (void)t;
    return which < VTERM_PALETTE ? vterm_palette[which]
                                   : vterm_specials[which - VTERM_PALETTE];
}

void vterm_reset_colors(term_t *t)
{
    for (unsigned i = 0; i < VTERM_COLORS; i++)
        vterm_reset_color(t, i);
}

void vterm_reset(term_t *t)
{
    vterm_smoothing.grid = NULL;
    vterm_reset_colors(t);
    draw_reset();
}
