/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/manifold.h"
#include "core/term/cells.h"
#include "core/term/color.h"
#include "core/term/grid.h"
#include "core/term/screen.h"
#include "core/term/term.h"
#include "core/vga/mode/mode0.h"
#include "core/vga/vga.h"
#include "machine.h"
#include <string.h>

// The console's screen on a machine that draws it: the terminal (term.c),
// followed at 40 and 80 columns, each size's screens keeping their cells in
// the cell store (screen.c), and what mode 0 draws the size showing from.
// This file gives the screens mode 0's cells, writes mode 0's words, keeps
// the palette and the colors a program sets, the blinks and the smooth
// scrolls, feeds the console's bytes to the terminal, and saves all of it.

#define CELLS_FG_COLOR_INDEX 7
#define CELLS_BG_COLOR_INDEX 0

// A blink is a thing on the screen, so it keeps the screen's time: these are
// frames, counted by vga_frame_count(), not microseconds. Every machine
// answers that tick, so a blink is the same blink on all of them -- and a
// period counted in frames cannot beat against the refresh it is drawn on,
// which is what the microsecond versions of these had to be detuned to avoid.

// Cell-blink base tick = the SGR 6 rapid-blink half-period. The phase counter
// (cell_blink_phase) increments every tick: bit 0 flips each tick (SGR 6 rapid,
// ~3 Hz), bit 1 every two ticks (SGR 5 slow, ~1.5 Hz -- the original rate).
// ~3 Hz clears ECMA-48's 150/min (2.5 Hz) slow/rapid boundary.
#define TERM_BLINK_TICK_FRAMES 10

// The frame count as the blinks and the smooth scrolls keep it, in 32 bits
// on every machine, as mode 0's smooth_frame word has it. A time is compared
// with it by the sign of their difference, so the count wrapping is no
// matter.
static inline uint32_t cells_frame(void)
{
    return (uint32_t)vga_frame_count();
}

// Cursor blink half-period.
#define CELLS_CURSOR_BLINK_FRAMES 30

// Whenever input changes the cursor's logical position or its
// appearance (DECSCUSR style, DECTCEM visibility), the cursor is
// forced unlit and its next blink tick is armed this far out. Pure
// SGR/OSC sequences and color-only changes (OSC 12/112) leave the
// blink schedule untouched. The gap re-lights to the appropriate
// steady/blink state when it fires -- one frame, which is as soon as
// anything can be seen.
#define CELLS_CURSOR_INPUT_GAP_FRAMES 1

// A smooth scroll moving on one of a size's screens (DECSCLM): rows top to
// bot, one way, the row of memory that went out (ghost), and the first frame
// it shows moving. The ghost keeps its line size in its row of memory. The
// store ends it when the count of cancels of either screen of its size
// moves on from the one the scroll started at.
typedef struct
{
    bool on;
    uint8_t buf;
    uint8_t top, bot;
    bool up;
    uint8_t ghost;
    uint32_t frame;
    uint32_t cancels;
} cells_smooth_t;

// What one size shows besides its cells: its smooth scroll and its blinks.
typedef struct
{
    cells_smooth_t smooth;
    uint32_t cursor_frame;
    bool cursor_lit;
    uint32_t cell_blink_frame;
    uint8_t cell_blink_phase;
} cells_draw_t;

// The terminal at one size, the rings of its screens and the line
// attributes of their rows of memory, and their cells: the primary's, then
// the alternate's, which a build without TERM_ALT_SCREEN does not give its
// screen.
typedef struct
{
    term_grid_t grid;
    uint8_t lines[2 * TERM_MEM_ROWS];
    uint8_t row_idx[2 * TERM_MAX_HEIGHT];
    screen_cells_t bufs[2];
} cells_t;

term_t cells_term;
static cells_t cells_40;
static cells_t cells_80;
static cells_draw_t draw_40;
static cells_draw_t draw_80;
/* Something mode 0 draws changed outside a batch of the console's bytes,
 * for cells_task to publish. */
static bool cells_stale;
static uint16_t cells_cursor_color; // OSC 12

// The layout the row words were last written from: the screen showing,
// which row of memory each of its rows has, and the line size of each row
// of memory. Only a scroll, a line size, a change of height, or another
// screen showing changes it, so most bytes leave the row words as they are.
static struct
{
    const screen_t *screen;
    uint8_t y_offset;
    uint8_t height;
    uint8_t row_idx[TERM_MAX_HEIGHT];
    uint8_t line[TERM_MEM_ROWS];
} cells_published;

// What a size of the store shows besides its cells.
static inline cells_draw_t *cells_draw(const cells_t *cells)
{
    return cells == &cells_80 ? &draw_80 : &draw_40;
}

// The size of the store a grid is, or NULL for a program's own size
// (term_add_grid), which is not drawn.
static inline cells_t *cells_size(const term_grid_t *g)
{
    return g == &cells_80.grid ? &cells_80 : g == &cells_40.grid ? &cells_40 : NULL;
}

// The store's count of cancels of a size's screens: a scroll, an index, a
// clear or a change of height of either moves it on.
static inline uint32_t cells_smooth_cancels(const cells_t *cells)
{
    return cells->bufs[0].cancels + cells->bufs[1].cancels;
}

// A smooth scroll is moving on the size: it started and the store has not
// ended it since.
static inline bool cells_smoothing(const cells_t *cells)
{
    const cells_smooth_t *sm = &cells_draw(cells)->smooth;
    return sm->on && sm->cancels == cells_smooth_cancels(cells);
}

// A smooth scroll the store ended is put away.
static void cells_smooth_ended(const cells_t *cells)
{
    cells_smooth_t *sm = &cells_draw(cells)->smooth;
    if (sm->cancels != cells_smooth_cancels(cells))
        sm->on = false;
}

/* ---- savestate ----------------------------------------------------------
 *
 * The terminal, each size's grid, the layout of each screen's cells and the
 * blinks, then the cursor's color. The cells, their colors and the palette
 * are mode 0's (mode0.h). The cells' addresses are never written, because
 * they are mode 0's, the same in every build of a machine. */

static bool cells_sst_term_ok(sst_cursor_t *c)
{
    return term_sst_ok(c, &cells_term);
}

static void cells_sst_put_alt(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_bool(c, cells_term.alt_active);
}

/* The alternate screen only exists in a build that has one. */
static bool cells_sst_get_alt(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    bool alt = sst_get_bool(c);
    if (!sst_ok(c))
        return false;
    if (alt && !TERM_ALT_SCREEN)
        return sst_refuse(c, "the alternate screen, which this build lacks");
    term_show(&cells_term, alt);
    return true;
}

/* A grid keeps its rows and its width, which are this file's. */
static bool cells_sst_grids_ok(sst_cursor_t *c)
{
    if (cells_40.grid.width != 40 || cells_80.grid.width != 80)
        return sst_refuse(c, "a screen of other columns");
    return term_grid_sst_ok(c, &cells_40.grid, TERM_MAX_HEIGHT) &&
           term_grid_sst_ok(c, &cells_80.grid, TERM_MAX_HEIGHT);
}

/* The line attributes, once the layouts below have loaded them. */
static bool cells_sst_lines_ok(sst_cursor_t *c)
{
    return term_grid_sst_lines_ok(c, &cells_40.grid) &&
           term_grid_sst_lines_ok(c, &cells_80.grid);
}

/* Each screen's layout, as the store puts it: each size's screens that have
 * cells. A put does a scroll the screen is owed first, which moves cells
 * that MODE0_DRIVER saves after this row. The counts of cancels are not
 * saved: a smooth scroll the store ended is put away before the save, and a
 * load starts the ones it brings back from the store's count as it is,
 * in the smooth scroll's row below. */
static void cells_sst_put_bufs(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    cells_t *const all[] = {&cells_40, &cells_80};
    for (int n = 0; n < 2; n++)
    {
        cells_smooth_ended(all[n]);
        for (int b = 0; b < MODE0_SCREENS; b++)
            screen_sst_put(c, &all[n]->grid.screen[b]);
    }
}

static bool cells_sst_get_bufs(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    cells_t *const all[] = {&cells_40, &cells_80};
    for (int n = 0; n < 2; n++)
        for (int b = 0; b < MODE0_SCREENS; b++)
            if (!screen_sst_get(c, &all[n]->grid.screen[b]))
                return false;
    return true;
}

#define CELLS_BLINK_SST_ROWS(Y, s)                                     \
    Y(U32, s.cursor_frame) Y(BOOL, s.cursor_lit)                       \
    Y(U32, s.cell_blink_frame) Y(U8, s.cell_blink_phase)               \
    Y(BOOL, s.smooth.on) Y(U8, s.smooth.buf)                           \
    Y(U8, s.smooth.top) Y(U8, s.smooth.bot) Y(BOOL, s.smooth.up)       \
    Y(U8, s.smooth.ghost)                                              \
    Y(U32, s.smooth.frame)

/* The screen of a smooth scroll, on or not, indexes the store's screens,
 * and the ghost and the rows of one that is on index the cells and the row
 * words. It counts its cancels from the store's count as it is. */
static bool cells_sst_smooth_ok(sst_cursor_t *c)
{
    cells_smooth_t *const all[] = {&draw_40.smooth, &draw_80.smooth};
    cells_t *const sizes[] = {&cells_40, &cells_80};
    for (int n = 0; n < 2; n++)
    {
        cells_smooth_t *sm = all[n];
        if (sm->buf >= MODE0_SCREENS ||
            (sm->on && (sm->top > sm->bot || sm->bot >= TERM_MAX_HEIGHT ||
                        sm->ghost >= TERM_MEM_ROWS)))
            return sst_refuse(c, "a smooth scroll out of range");
        sm->cancels = cells_smooth_cancels(sizes[n]);
    }
    return true;
}

#define CELLS_SST(X)                                                   \
    TERM_SST_ROWS(X, cells_term)                                       \
    X(VALIDATE, cells_sst_term_ok)                                     \
    X(FN, cells_sst_put_alt, cells_sst_get_alt, 1, 1)                  \
    X(IN_FN, cells_term.alt_active)                                    \
    X(SKIP, cells_term.grids, "the sizes it is followed at")           \
    X(STRUCT, cells_40.grid, TERM_GRID_SST_ROWS)                       \
    X(STRUCT, cells_80.grid, TERM_GRID_SST_ROWS)                       \
    X(VALIDATE, cells_sst_grids_ok)                                    \
    X(FN, cells_sst_put_bufs, cells_sst_get_bufs,                      \
      2 * MODE0_SCREENS * SCREEN_SST_SIZE, 2)                       \
    X(IN_FN, cells_40.lines)                                           \
    X(IN_FN, cells_40.row_idx)                                         \
    X(IN_FN, cells_40.bufs)                                            \
    X(IN_FN, cells_80.lines)                                           \
    X(IN_FN, cells_80.row_idx)                                         \
    X(IN_FN, cells_80.bufs)                                            \
    X(VALIDATE, cells_sst_lines_ok)                                    \
    X(STRUCT, draw_40, CELLS_BLINK_SST_ROWS)                           \
    X(STRUCT, draw_80, CELLS_BLINK_SST_ROWS)                           \
    X(VALIDATE, cells_sst_smooth_ok)                                   \
    X(U16, cells_cursor_color)                                         \
    X(SKIP, cells_stale, "the canvas's load publishes everything")     \
    X(SKIP, cells_published, "what the row words were last written "   \
                             "from, which the canvas's load compares")
SST_TABLE(cells, CELLS_SST)

// Force the cursor unlit and arm the input-gap timer. Used by movement
// and by appearance changes (DECSCUSR, DECTCEM enable) so the cursor's new
// state lands on a clean blink boundary.
static inline void cells_cursor_restart_blink(cells_draw_t *d)
{
    d->cursor_lit = false;
    d->cursor_frame = cells_frame() + CELLS_CURSOR_INPUT_GAP_FRAMES;
}

// Restart the cursor blink and the blinking text together, both lit, so a
// reset leaves them in step.
static void cells_restart_blinks(cells_draw_t *d)
{
    cells_cursor_restart_blink(d);
    d->cell_blink_phase = 0;
    d->cell_blink_frame = cells_frame() + TERM_BLINK_TICK_FRAMES;
}

static cells_t *cells_visible(void);

// The store kept the row that went (screen_scroll, screen_index), so a smooth
// scroll is only recorded, for the size showing. Without a spare row of
// memory a region's row going out is erased as it goes, so nothing is drawn
// moving.
void cells_smooth(term_t *term, term_grid_t *g, uint8_t top, uint8_t bot, bool up,
                  uint8_t ghost)
{
    (void)term;
    cells_t *cells = cells_size(g);
    if (!TERM_SPARE_ROWS || !cells || cells != cells_visible())
        return;
    const uint8_t buf = (uint8_t)(g->shown - g->screen);
    cells_draw(cells)->smooth = (cells_smooth_t){
        .on = true,
        .buf = buf,
        .top = top,
        .bot = bot,
        .up = up,
        .ghost = ghost,
        .frame = cells_frame() + 1,
        .cancels = cells_smooth_cancels(cells),
    };
}

bool cells_smooth_busy(const term_grid_t *g)
{
    const cells_t *cells = cells_size(g);
    return cells && cells_smoothing(cells) &&
           (int32_t)(cells_frame() - (cells_draw(cells)->smooth.frame + TERM_SMOOTH_FRAMES -
                                      1)) < 0;
}

/* ---- the palette --------------------------------------------------------
 *
 * A palette entry or default color that changes reaches every cell drawn
 * with it, as on xterm and Linux, because those cells hold the entry and
 * not the color. */

// The blinks start again and a smooth scroll stops, for both sizes, as the
// palette and the colors do.
void cells_colors_reset(term_t *term)
{
    (void)term;
    cells_draw_t *const all[] = {&draw_40, &draw_80};
    for (int n = 0; n < 2; n++)
    {
        all[n]->smooth.on = false;
        cells_restart_blinks(all[n]);
    }
    for (unsigned i = 0; i < 256; i++)
        mode0_set_color(i, color_256[i]);
    mode0_set_color(MODE0_DEFAULT_FG, color_256[CELLS_FG_COLOR_INDEX]);
    mode0_set_color(MODE0_DEFAULT_BG, color_256[CELLS_BG_COLOR_INDEX]);
    cells_cursor_color = color_256[CELLS_FG_COLOR_INDEX];
}

void cells_color(term_t *term, unsigned which, uint16_t color)
{
    (void)term;
    if (which < 256)
        mode0_set_color(which, color);
    else if (which == CELLS_COLOR_FG)
        mode0_set_color(MODE0_DEFAULT_FG, color);
    else if (which == CELLS_COLOR_BG)
        mode0_set_color(MODE0_DEFAULT_BG, color);
    else if (which == CELLS_COLOR_CURSOR)
        cells_cursor_color = color;
}

uint16_t cells_color_get(const term_t *term, unsigned which)
{
    (void)term;
    if (which < 256)
        return mode0_palette[which];
    if (which == CELLS_COLOR_CURSOR)
        return cells_cursor_color;
    return mode0_palette[which == CELLS_COLOR_BG ? MODE0_DEFAULT_BG : MODE0_DEFAULT_FG];
}

void cells_color_reset(term_t *term, unsigned which)
{
    if (which < 256)
        cells_color(term, which, color_256[which]);
    else
        cells_color(term, which, color_256[which == CELLS_COLOR_BG ? CELLS_BG_COLOR_INDEX
                                                                             : CELLS_FG_COLOR_INDEX]);
}

/* ---- the stream -------------------------------------------------------- */

static cells_t *cells_visible(void)
{
    return vga_canvas_width() == 320 ? &cells_40 : &cells_80;
}

// n bytes compared a word at a time, only the bits of each byte in mask.
static inline bool cells_bytes_differ(const uint8_t *a, const uint8_t *b, unsigned n,
                                      uint8_t mask)
{
    uint32_t diff = 0;
    unsigned i = 0;
    for (; i + 4 <= n; i += 4)
    {
        uint32_t x, y;
        memcpy(&x, a + i, 4);
        memcpy(&y, b + i, 4);
        diff |= x ^ y;
    }
    diff &= mask * 0x01010101u;
    for (; i < n; i++)
        diff |= (uint32_t)((a[i] ^ b[i]) & mask);
    return diff != 0;
}

void cells_publish(void)
{
    cells_t *cells = cells_visible();
    cells_draw_t *d = cells_draw(cells);
    const screen_t *s = cells->grid.shown;
    if (cells_published.screen != s || cells_published.y_offset != s->y_offset ||
        cells_published.height != cells->grid.height ||
        cells_bytes_differ(cells_published.row_idx, s->row_idx, TERM_MAX_HEIGHT, 0xFF) ||
        cells_bytes_differ(cells_published.line, s->line, TERM_MEM_ROWS, SCREEN_LINE_SIZE))
    {
        cells_published.screen = s;
        cells_published.y_offset = s->y_offset;
        cells_published.height = cells->grid.height;
        memcpy(cells_published.row_idx, s->row_idx, TERM_MAX_HEIGHT);
        memcpy(cells_published.line, s->line, TERM_MEM_ROWS);
        for (uint8_t y = 0; y < cells->grid.height; y++)
            mode0_regs.row[y] = MODE0_ROW(screen_row(s, y) - mode0_cells, screen_size(s, y));
    }
    const term_pos_t *pos = term_pos(&cells->grid);
    // A cursor with a wrap pending is on the column it wrote, as on a VT100.
    mode0_regs.cursor = MODE0_CURSOR(pos->x, pos->y, cells_term.cursor_style) |
                        (cells_term.cursor_enabled ? MODE0_CURSOR_ENABLED : 0) |
                        (d->cursor_lit ? MODE0_CURSOR_LIT : 0);
    mode0_regs.cursor_color = cells_cursor_color;
    mode0_regs.screen = d->cell_blink_phase |
                        (cells_term.reverse_screen ? MODE0_SCREEN_REVERSE : 0);
    cells_smooth_t *sm = &d->smooth;
    const bool smooth = cells_smoothing(cells) && sm->buf == (uint8_t)(s - cells->grid.screen);
    if (smooth)
    {
        const screen_cells_t *b = s->cells;
        mode0_regs.smooth_ghost =
            MODE0_ROW(b->mem + (uint32_t)sm->ghost * b->width - mode0_cells,
                      screen_mem_size(s, sm->ghost));
        mode0_regs.smooth_frame = (uint32_t)sm->frame;
        mode0_regs.smooth = MODE0_SMOOTH(sm->top, sm->bot, sm->up);
    }
    else
        mode0_regs.smooth = 0;
    // The words now describe one picture.
    mode0_regs.publish = 1;
    cells_stale = false;
    screen_moved = false;
    // A fabric that met a frame start part way through the publish kept the
    // last frame's words for that frame, and reads the publish word back 0.
    // When that frame was a smooth scroll's first, no frame has shown the
    // scroll on its way, so it starts a frame later and goes again.
    if (smooth && !mode0_regs.publish && cells_frame() == sm->frame)
    {
        sm->frame++;
        cells_stale = true;
    }
}

void cells_reply(const term_t *term, const char *s, size_t n)
{
    (void)term;
    manifold_reply(s, n);
}

void cells_bell(const term_t *term)
{
    (void)term;
    manifold_bell();
}

term_grid_t *cells_shown(const term_t *term)
{
    (void)term;
    return &cells_visible()->grid;
}

// Every byte goes to the terminal, which follows it at both sizes, up to
// one that has to wait for a smooth scroll still moving (term_holds), unless
// force. Returns how many went. A cursor's blink starts again when a byte
// moves it or changes how it looks: its style, or it coming back on.
static int cells_out_chars(const char *buf, int length, bool force)
{
    term_t *term = &cells_term;
    cells_t *const all[2] = {&cells_40, &cells_80};
    int i = 0;
    for (; i < length; i++)
    {
        if (!force && term_holds(term, (uint8_t)buf[i]))
            break;
        term_pos_t was[2];
        for (int n = 0; n < 2; n++)
            was[n] = *term_pos(&all[n]->grid);
        uint8_t ps = term->cursor_style;
        bool pe = term->cursor_enabled;
        term_write(term, (uint8_t)buf[i]);
        bool look = term->cursor_style != ps || (term->cursor_enabled && !pe);
        for (int n = 0; n < 2; n++)
        {
            const term_pos_t *now = term_pos(&all[n]->grid);
            if (look || now->x != was[n].x || now->y != was[n].y ||
                now->wrap_pending != was[n].wrap_pending)
                cells_cursor_restart_blink(cells_draw(all[n]));
        }
    }
    cells_publish();
    return i;
}

// The cells are mode 0's: each size's primary, then its alternate.
static void cells_give_cells(void)
{
    mode0_cell_t *mem = mode0_cells;
    cells_t *const all[] = {&cells_40, &cells_80};
    for (int n = 0; n < 2; n++)
        for (int b = 0; b < MODE0_SCREENS; b++)
        {
            all[n]->bufs[b].mem = mem;
            all[n]->bufs[b].width = n ? 80 : 40;
            mem += all[n]->bufs[b].width * TERM_MEM_ROWS;
        }
}

/* New grids at both sizes, their screens laid out in this file's line sizes
 * and rings and given their cells. Without an alternate store, the
 * alternate screen has no cells, and the terminal never switches to it. */
static void cells_new_grids(void)
{
    cells_t *const all[] = {&cells_40, &cells_80};
    for (int n = 0; n < 2; n++)
    {
        term_grid_init(&all[n]->grid, n ? 80 : 40, all[n]->lines, all[n]->row_idx,
                       TERM_MAX_HEIGHT);
        for (int b = 0; b < MODE0_SCREENS; b++)
            screen_set_cells(&all[n]->grid.screen[b], &all[n]->bufs[b]);
    }
}

void cells_init(void)
{
    cells_give_cells();
    cells_restart();
    // become part of stdout
    manifold_set_term_out(cells_out_chars);
}

void cells_restart(void)
{
    /* Every cell is a blank in the default colors at once, the scrollback
     * too, so no glyph or color a load left can reach the renderer. */
    screen_blank_cells(mode0_cells, MODE0_CELLS);
    /* New grids, and a new terminal on them, whose RIS lays out every
     * screen's cells again, stops a smooth scroll, restarts the blinks and
     * resets the palette. A program's own grid is dropped with the rest. */
    cells_new_grids();
    term_grid_t *const grids[] = {&cells_40.grid, &cells_80.grid};
    term_init(&cells_term, grids, 2);
    cells_stale = true;
}

// Cursor blink timing only — toggles cursor_lit on its cadence and forces
// unlit when the cursor is disabled. Steady-vs-blink handling lives in the renderers,
// which ignore cursor_lit when cursor_style is steady; this keeps the
// timing path style-agnostic.
// True when cursor_lit changed.
static bool cells_blink_cursor(cells_draw_t *d)
{
    bool was = d->cursor_lit;
    if (!cells_term.cursor_enabled)
        d->cursor_lit = false;
    else if ((int32_t)(cells_frame() - d->cursor_frame) >= 0)
    {
        d->cursor_lit = !d->cursor_lit;
        d->cursor_frame = cells_frame() + CELLS_CURSOR_BLINK_FRAMES;
    }
    return d->cursor_lit != was;
}

// SGR-5/6 cell blink phase, per size: a free-running 2-bit counter at bits 0-1
// (TERM_ATTR_ANY_BLINK). Bit 0 (TERM_ATTR_BLINK_FAST) flips every tick -> SGR 6 rapid;
// bit 1 (TERM_ATTR_BLINK) every two ticks -> SGR 5 slow. The renderer ANDs a cell's
// single blink bit against this value, so each cell darkens only on its own
// rate's off-phase, and both relight together at phase 0. Per size so the
// cursor timer and cell blink timer share an owner instead of crossing a
// global. True when the phase changed.
static bool cells_cell_blink_phase_task(cells_draw_t *d)
{
    if ((int32_t)(cells_frame() - d->cell_blink_frame) < 0)
        return false;
    d->cell_blink_phase = (uint8_t)((d->cell_blink_phase + 1) & TERM_ATTR_ANY_BLINK);
    d->cell_blink_frame = cells_frame() + TERM_BLINK_TICK_FRAMES;
    return true;
}

// A smooth scroll that has finished is put away, so the frame count wrapping
// round long after can never show it again. One the store ended is put away
// too, without a publish of its own.
static bool cells_smooth_task(cells_t *cells)
{
    cells_smooth_ended(cells);
    cells_smooth_t *sm = &cells_draw(cells)->smooth;
    if (!sm->on || (int32_t)(cells_frame() - sm->frame) < TERM_SMOOTH_FRAMES)
        return false;
    sm->on = false;
    return true;
}

// Do a scroll between left and right margins either screen of a size is
// owed, or else pay one row at most what it is owed: the screen showing
// first, and the other only once the one showing is clean. The cursor's row
// of the screen showing never owes anything (the terminal touches every row
// it puts the cursor on), so this never changes the cell under the cursor
// and never has to restart its blink.
static void cells_tidy(term_grid_t *g)
{
    for (int b = 0; b < 2; b++)
        if (g->screen[b].cells && g->screen[b].cells->later.n)
        {
            screen_settle(&g->screen[b]);
            return;
        }
    if (screen_tidy(g->shown, g->height))
        return;
    screen_t *other = &g->screen[g->shown == &g->screen[0] ? 1 : 0];
    if (other->cells)
        screen_tidy(other, g->height);
}

void cells_task(void)
{
    cells_stale |= cells_smooth_task(&cells_40);
    cells_stale |= cells_smooth_task(&cells_80);
    cells_stale |= cells_blink_cursor(&draw_40);
    cells_stale |= cells_blink_cursor(&draw_80);
    cells_stale |= cells_cell_blink_phase_task(&draw_40);
    cells_stale |= cells_cell_blink_phase_task(&draw_80);
    if (cells_stale || screen_moved)
        cells_publish();
    cells_tidy(&cells_40.grid);
    cells_tidy(&cells_80.grid);
}

void cells_RIS(void)
{
    term_RIS(&cells_term);
    cells_stale = true;
}

void cells_RIS_no_clear(void)
{
    term_set_height(&cells_term, &cells_40.grid, TERM_STD_HEIGHT);
    term_RIS_no_clear(&cells_term);
    cells_stale = true;
}

void cells_set_height(uint8_t width, uint8_t height)
{
    term_set_height(&cells_term, width == 40 ? &cells_40.grid : &cells_80.grid, height);
    cells_stale = true;
}
