/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/draw.h"
#include "core/term/color.h"
#include "core/term/font.h"
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
vterm_placement_t vterm_placement;
screen_selection_t vterm_selection;
screen_marks_t vterm_marks;
uint16_t vterm_palette[VTERM_PALETTE];
uint16_t vterm_specials[VTERM_SPECIALS];
bool vterm_palette_changed;
static uint16_t vterm_code_page;

/* draw.c's CELL chunk saves the terminal, both sizes, the smooth scroll,
 * where mode 0 sits, the selection, the palette and the special colors, and
 * font.c's FONT chunk the code page, with the glyphs it writes. */
#define VTERM_SST(X)                                             \
    X(SKIP, vterm, "draw.c's CELL chunk saves it")               \
    X(SKIP, vterm_40, "draw.c's CELL chunk saves it")            \
    X(SKIP, vterm_80, "draw.c's CELL chunk saves it")            \
    X(SKIP, vterm_smoothing, "draw.c's CELL chunk saves it")     \
    X(SKIP, vterm_placement, "draw.c's CELL chunk saves it")     \
    X(SKIP, vterm_selection, "draw.c's CELL chunk saves it")     \
    X(SKIP, vterm_marks, "the console's mouse keeps its own "    \
                         "points in them, and saves those")      \
    X(SKIP, vterm_palette, "draw.c's CELL chunk saves it")       \
    X(SKIP, vterm_specials, "draw.c's CELL chunk saves it")      \
    X(SKIP, vterm_palette_changed, "draw.c follows every color " \
                                   "after a load")               \
    X(SKIP, vterm_code_page, "font.c's FONT chunk saves it")
SST_TABLE(vterm, VTERM_SST)

/* The terminal a console with no VGA is taken to be on the UART. */
#define VTERM_SERIAL_ROWS 24

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
            all[n]->bufs[b] = (screen_cells_t){.mem = cells, .width = n ? 80 : 40};
            cells += all[n]->bufs[b].width * TERM_MEM_ROWS;
            screen_set_cells(&all[n]->grid.screen[b], &all[n]->bufs[b]);
        }
    }
    term_grid_t *const grids[] = {&vterm_40.grid, &vterm_80.grid};
    term_init(&vterm, grids, 2);
    vterm_placement = (vterm_placement_t){.cols = 80, .console = true};
    vterm_selection = (screen_selection_t){.changed = true};
}

void vterm_RIS(void)
{
    term_job_finish(&vterm);
    term_RIS(&vterm);
    screen_selection_drop();
}

void vterm_RIS_no_clear(void)
{
    term_job_finish(&vterm);
    term_set_height(&vterm, &vterm_40.grid, TERM_STD_HEIGHT);
    term_RIS_no_clear(&vterm);
}

/* ---- where mode 0 sits ------------------------------------------------- */

// The record becomes p, and a selection goes when that changes it.
static void vterm_placement_set(const vterm_placement_t *p)
{
    vterm_placement_t *was = &vterm_placement;
    if (was->begin == p->begin && was->cols == p->cols && was->rows == p->rows &&
        was->row_px == p->row_px && was->console == p->console && was->placed == p->placed)
        return;
    *was = *p;
    screen_selection_drop();
}

void vterm_canvas(uint8_t cols, bool is_console)
{
    if (is_console && vterm_placement.console)
        return;
    vterm_placement_set(&(vterm_placement_t){.cols = cols, .console = is_console});
}

void vterm_place(uint8_t cols, uint8_t rows, int16_t begin, uint8_t row_px)
{
    vterm_placement_set(&(vterm_placement_t){.begin = begin,
                                             .cols = cols,
                                             .rows = rows,
                                             .row_px = row_px,
                                             .console = vterm_placement.console,
                                             .placed = true});
    term_job_finish(&vterm);
    term_set_height(&vterm, cols == 40 ? &vterm_40.grid : &vterm_80.grid, rows);
}

void vterm_unplace(void)
{
    vterm_placement = (vterm_placement_t){.cols = 80};
    screen_selection_drop();
    term_job_finish(&vterm);
    term_set_height(&vterm, &vterm_80.grid, VTERM_SERIAL_ROWS);
}

/* ---- the selection ----------------------------------------------------- */

// The selection's grid, the one whose screen it is on.
static const term_grid_t *vterm_selection_grid(void)
{
    const screen_t *s = vterm_selection.screen;
    return s == &vterm_80.grid.screen[0] || s == &vterm_80.grid.screen[1] ? &vterm_80.grid
                                                                          : &vterm_40.grid;
}

// The rows of memory the selection takes, walking g's rows of the height
// from its start to its end. False when its ends are not rows of the
// height in that order, its columns are more than their rows have, or its
// sides have no column between them.
static bool vterm_selection_lay(const term_grid_t *g)
{
    screen_selection_t *v = &vterm_selection;
    const screen_t *s = v->screen;
    bool in = false;
    v->rows = 0;
    for (uint8_t y = 0; y < g->height; y++)
    {
        const uint8_t m = screen_mem(s, y);
        const unsigned cols = screen_mem_size(s, m) ? g->width / 2u : g->width;
        if (m == v->start_row)
        {
            if (in || v->start_col > cols)
                return false;
            in = true;
        }
        if (!in)
            continue;
        v->rows |= 1ull << m;
        if (m == v->end_row)
            return v->end_col <= cols && v->left < v->right &&
                   (m != v->start_row || v->start_col < v->end_col);
    }
    return false;
}

// The selection, as the user or a save state leaves it. False when the
// ends select nothing but are not the same.
static bool vterm_selection_set(const term_grid_t *g, uint8_t screen, uint8_t start_row,
                                uint8_t start_col, uint8_t end_row, uint8_t end_col,
                                uint8_t left, uint8_t right)
{
    screen_selection_t *v = &vterm_selection;
    v->screen = &g->screen[screen];
    v->start_row = start_row;
    v->start_col = start_col;
    v->end_row = end_row;
    v->end_col = end_col;
    v->left = left;
    v->right = right;
    v->changed = true;
    v->shows = false;
    if (start_row == end_row && start_col == end_col)
        return true;
    v->shows = screen < VTERM_SCREENS && vterm_selection_lay(g);
    return v->shows;
}

void vterm_select(const term_grid_t *g, uint8_t screen, uint8_t start_row, uint8_t start_col,
                  uint8_t end_row, uint8_t end_col, uint8_t left, uint8_t right)
{
    vterm_selection.unsent = true;
    vterm_selection_set(g, screen, start_row, start_col, end_row, end_col, left, right);
}

void vterm_unselect(void)
{
    vterm_selection.unsent = true;
    screen_selection_drop();
}

#define VTERM_SELECT_WORD(code, row, col) \
    ((uint16_t)((unsigned)(col) | (unsigned)(row) << 7 | (unsigned)(code) << 13))
#define VTERM_SELECT_COL(w) ((uint8_t)((w) & 0x7F))
#define VTERM_SELECT_ROW(w) ((uint8_t)(((w) >> 7) & 0x3F))
#define VTERM_SELECT_CODE(w) ((unsigned)(w) >> 13)
#define VTERM_SELECT_SIDES(left, right) ((uint16_t)((unsigned)(left) | (unsigned)(right) << 7))
#define VTERM_SELECT_LEFT(w) ((uint8_t)((w) & 0x7F))
#define VTERM_SELECT_RIGHT(w) ((uint8_t)(((w) >> 7) & 0x7F))
_Static_assert(TERM_MEM_ROWS <= 64 && SCREEN_SELECTION_EDGE <= 0x7F,
               "a row of memory and a column in a word");

vterm_select_words_t vterm_select_words(void)
{
    const screen_selection_t *v = &vterm_selection;
    if (!v->shows)
        return (vterm_select_words_t){0, 0, 0};
    const term_grid_t *g = vterm_selection_grid();
    const unsigned code = (g == &vterm_80.grid) << 1 | (unsigned)(v->screen - g->screen);
    return (vterm_select_words_t){
        VTERM_SELECT_WORD(code, v->start_row, v->start_col),
        VTERM_SELECT_SIDES(v->left, v->right),
        VTERM_SELECT_WORD(code, v->end_row, v->end_col),
    };
}

bool vterm_select_apply(vterm_select_words_t w)
{
    const unsigned code = VTERM_SELECT_CODE(w.start);
    if (w.start != w.end && (code > 3 || code != VTERM_SELECT_CODE(w.end) || w.sides >> 14))
        return false;
    return vterm_selection_set(code >> 1 ? &vterm_80.grid : &vterm_40.grid, code & 1,
                               VTERM_SELECT_ROW(w.start), VTERM_SELECT_COL(w.start),
                               VTERM_SELECT_ROW(w.end), VTERM_SELECT_COL(w.end),
                               VTERM_SELECT_LEFT(w.sides), VTERM_SELECT_RIGHT(w.sides));
}

uint16_t vterm_get_code_page(void)
{
    return vterm_code_page;
}

void vterm_load_code_page(uint16_t cp)
{
    vterm_code_page = font_code_page_of(cp);
}

bool vterm_set_code_page(uint16_t cp)
{
    uint16_t was = vterm_code_page;
    vterm_load_code_page(cp);
    if (vterm_code_page == was)
        return false;
    vterm_RIS();
    return true;
}

term_grid_t *vterm_get_shown(const term_t *t)
{
    (void)t;
    return vga_connected() && vterm_placement.cols == 40 ? &vterm_40.grid : &vterm_80.grid;
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
    uint16_t *const cursor = &vterm_specials[VTERM_COLOR_CURSOR - VTERM_PALETTE];
    /* As on xterm, a cursor color that is the default foreground, as it
     * starts, stays it when the default foreground changes. */
    if (which == VTERM_COLOR_FG && *cursor == vterm_specials[VTERM_COLOR_FG - VTERM_PALETTE])
        *cursor = color;
    if (which < VTERM_PALETTE)
        vterm_palette[which] = color;
    else
        vterm_specials[which - VTERM_PALETTE] = color;
    vterm_palette_changed = true;
}

uint16_t vterm_default_color(unsigned which)
{
    return color_256[which < VTERM_PALETTE ? which
                     : which == VTERM_COLOR_BG || which == VTERM_COLOR_HIGHLIGHT_FG
                         ? VTERM_BG_COLOR_INDEX
                         : VTERM_FG_COLOR_INDEX];
}

void vterm_reset_color(term_t *t, unsigned which)
{
    vterm_set_color(t, which, vterm_default_color(which));
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
