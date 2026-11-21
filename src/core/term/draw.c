/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/manifold.h"
#include "core/term/cells.h"
#include "core/term/console_term.h"
#include "core/term/grid.h"
#include "core/term/screen.h"
#include "core/term/term.h"
#include "core/vga/mode/mode0.h"
#include "core/vga/vga.h"
#include "machine.h"
#include <string.h>

// The console's screen on a machine that draws it: the console's terminal
// (console_term.c), each size's screens keeping their cells in the cell
// store (screen.c), and what mode 0 draws the size showing from. This file
// hands the console mode 0's cells, writes mode 0's words and the faint
// twins of the console's palette, keeps the blinks, draws the smooth
// scrolls, feeds the console's bytes to the terminal, and saves all of it.

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

// What one size shows besides its cells: its blinks.
typedef struct
{
    uint32_t cursor_frame;
    bool cursor_lit;
    uint32_t cell_blink_frame;
    uint8_t cell_blink_phase;
} cells_draw_t;

static cells_draw_t draw_40;
static cells_draw_t draw_80;
/* Something mode 0 draws changed outside a batch of the console's bytes,
 * for cells_task to publish. */
static bool cells_stale;

// The layout the row words were last written from: the screen showing,
// which row of memory each of its rows has, and the line size of each row
// of memory. Only a scroll, a line size, a change of height, or another
// screen showing changes it, so most bytes leave the row words as they are.
// And whether the words showed a smooth scroll moving.
static struct
{
    bool smooth;
    const screen_t *screen;
    uint8_t y_offset;
    uint8_t height;
    uint8_t row_idx[TERM_MAX_HEIGHT];
    uint8_t line[TERM_MEM_ROWS];
} cells_published;

// What a size of the console shows besides its cells.
static inline cells_draw_t *cells_draw(const term_grid_t *g)
{
    return g == &console_80.grid ? &draw_80 : &draw_40;
}

// The smooth scroll moving on the screen showing of g, drawn where a screen
// keeps a spare row of memory for the row going out. Without one the row
// going out is erased as it goes, so nothing is drawn moving.
static bool cells_smooth_shows(const term_grid_t *g)
{
    return console_smooth_busy(g) && TERM_SPARE_ROWS &&
           console_smoothing.screen == (uint8_t)(g->shown - g->screen);
}

/* Mode 0's faint twins of the console's palette (mode0.h) follow it once a
 * color of it changes: all of them, as a change is rare and a reset or a
 * load changes them all. */
static void cells_follow_palette(void)
{
    if (!console_palette_changed)
        return;
    console_palette_changed = false;
    for (unsigned i = 0; i < MODE0_ENTRIES; i++)
        mode0_faint_palette[i] = mode0_faint(console_palette[i]);
}

/* ---- savestate ----------------------------------------------------------
 *
 * The console's terminal, each size's grid, the layout of each screen's
 * cells, the blinks and the smooth scroll, then the palette. The cells and
 * their colors are mode 0's (mode0.h). The cells' addresses are never
 * written, because they are mode 0's, the same in every build of a
 * machine. */

static bool cells_sst_term_ok(sst_cursor_t *c)
{
    return term_sst_ok(c, &console_term);
}

static void cells_sst_put_alt(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_bool(c, console_term.alt_active);
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
    term_show(&console_term, alt);
    return true;
}

/* A grid keeps its rows and its width, which are the console's. */
static bool cells_sst_grids_ok(sst_cursor_t *c)
{
    if (console_40.grid.width != 40 || console_80.grid.width != 80)
        return sst_refuse(c, "a screen of other columns");
    return term_grid_sst_ok(c, &console_40.grid, TERM_MAX_HEIGHT) &&
           term_grid_sst_ok(c, &console_80.grid, TERM_MAX_HEIGHT);
}

/* The line attributes, once the layouts below have loaded them. */
static bool cells_sst_lines_ok(sst_cursor_t *c)
{
    return term_grid_sst_lines_ok(c, &console_40.grid) &&
           term_grid_sst_lines_ok(c, &console_80.grid);
}

/* Each screen's layout, as the store puts it: each size's screens that have
 * cells. A put does a scroll the screen is owed first, which moves cells
 * that MODE0_DRIVER saves after this row. */
static void cells_sst_put_bufs(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    console_size_t *const all[] = {&console_40, &console_80};
    for (int n = 0; n < 2; n++)
        for (int b = 0; b < MODE0_SCREENS; b++)
            screen_sst_put(c, &all[n]->grid.screen[b]);
}

static bool cells_sst_get_bufs(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    console_size_t *const all[] = {&console_40, &console_80};
    for (int n = 0; n < 2; n++)
        for (int b = 0; b < MODE0_SCREENS; b++)
            if (!screen_sst_get(c, &all[n]->grid.screen[b]))
                return false;
    return true;
}

#define CELLS_BLINK_SST_ROWS(Y, s)                                     \
    Y(U32, s.cursor_frame) Y(BOOL, s.cursor_lit)                       \
    Y(U32, s.cell_blink_frame) Y(U8, s.cell_blink_phase)

/* The smooth scroll's grid, as 0 for none, 1 for the 40 and 2 for the 80.
 * Its count of cancels is not saved: console_smooth_grid puts away one that
 * has landed or that its grid ended, so only one still moving is saved, and
 * a load starts the one it brings back from the grid's count as it is,
 * below. */
static void cells_sst_put_smooth(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    const term_grid_t *g = console_smooth_grid();
    sst_put_u8(c, g == &console_40.grid ? 1 : g == &console_80.grid ? 2 : 0);
}

static bool cells_sst_get_smooth(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    const uint8_t n = sst_get_u8(c);
    if (!sst_ok(c))
        return false;
    if (n > 2)
        return sst_refuse(c, "a smooth scroll of no size");
    console_smoothing.grid = n == 1 ? &console_40.grid : n == 2 ? &console_80.grid : NULL;
    return true;
}

#define CELLS_SMOOTH_SST_ROWS(Y, s)                                    \
    Y(U8, s.screen) Y(U8, s.top) Y(U8, s.bot) Y(BOOL, s.up)            \
    Y(U8, s.ghost) Y(U32, s.frame)

/* The screen of a smooth scroll, on or not, indexes the store's screens,
 * and the ghost and the rows of one that is on index the cells and the row
 * words. It counts its cancels from its grid's count as it is. */
static bool cells_sst_smooth_ok(sst_cursor_t *c)
{
    console_smooth_t *sm = &console_smoothing;
    if (sm->screen >= MODE0_SCREENS ||
        (sm->grid && (sm->top > sm->bot || sm->bot >= TERM_MAX_HEIGHT ||
                      sm->ghost >= TERM_MEM_ROWS)))
        return sst_refuse(c, "a smooth scroll out of range");
    if (sm->grid)
        sm->cancels = sm->grid->cancels;
    return true;
}

/* Any color is one, and mode 0's faint twins are made from the palette. */
static bool cells_sst_palette_ok(sst_cursor_t *c)
{
    (void)c;
    console_palette_changed = true;
    cells_follow_palette();
    return true;
}

#define CELLS_SST(X)                                                   \
    TERM_SST_ROWS(X, console_term)                                     \
    X(VALIDATE, cells_sst_term_ok)                                     \
    X(FN, cells_sst_put_alt, cells_sst_get_alt, 1, 1)                  \
    X(IN_FN, console_term.alt_active)                                  \
    X(SKIP, console_term.grids, "the sizes it is followed at")         \
    X(STRUCT, console_40.grid, TERM_GRID_SST_ROWS)                     \
    X(STRUCT, console_80.grid, TERM_GRID_SST_ROWS)                     \
    X(VALIDATE, cells_sst_grids_ok)                                    \
    X(FN, cells_sst_put_bufs, cells_sst_get_bufs,                      \
      2 * MODE0_SCREENS * SCREEN_SST_SIZE, 2)                          \
    X(IN_FN, console_40.lines)                                         \
    X(IN_FN, console_40.row_idx)                                       \
    X(IN_FN, console_40.bufs)                                          \
    X(IN_FN, console_80.lines)                                         \
    X(IN_FN, console_80.row_idx)                                       \
    X(IN_FN, console_80.bufs)                                          \
    X(VALIDATE, cells_sst_lines_ok)                                    \
    X(STRUCT, draw_40, CELLS_BLINK_SST_ROWS)                           \
    X(STRUCT, draw_80, CELLS_BLINK_SST_ROWS)                           \
    X(FN, cells_sst_put_smooth, cells_sst_get_smooth, 1, 1)            \
    X(IN_FN, console_smoothing.grid)                                   \
    X(STRUCT, console_smoothing, CELLS_SMOOTH_SST_ROWS)                \
    X(VALIDATE, cells_sst_smooth_ok)                                   \
    X(U16S, console_palette)                                           \
    X(VALIDATE, cells_sst_palette_ok)                                  \
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

// RIS and the system's soft reset: the blinks of both sizes start again,
// and everything is published.
void draw_reset(void)
{
    cells_restart_blinks(&draw_40);
    cells_restart_blinks(&draw_80);
    cells_stale = true;
}

/* ---- the stream -------------------------------------------------------- */

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
    cells_follow_palette();
    const term_grid_t *g = console_shown(&console_term);
    cells_draw_t *d = cells_draw(g);
    const screen_t *s = g->shown;
    if (cells_published.screen != s || cells_published.y_offset != s->y_offset ||
        cells_published.height != g->height ||
        cells_bytes_differ(cells_published.row_idx, s->row_idx, TERM_MAX_HEIGHT, 0xFF) ||
        cells_bytes_differ(cells_published.line, s->line, TERM_MEM_ROWS, SCREEN_LINE_SIZE))
    {
        cells_published.screen = s;
        cells_published.y_offset = s->y_offset;
        cells_published.height = g->height;
        memcpy(cells_published.row_idx, s->row_idx, TERM_MAX_HEIGHT);
        memcpy(cells_published.line, s->line, TERM_MEM_ROWS);
        for (uint8_t y = 0; y < g->height; y++)
            mode0_regs.row[y] = MODE0_ROW(screen_row(s, y) - mode0_cells, screen_size(s, y));
    }
    const term_pos_t *pos = term_pos(g);
    // A cursor with a wrap pending is on the column it wrote, as on a VT100.
    mode0_regs.cursor = MODE0_CURSOR(pos->x, pos->y, console_term.cursor_style) |
                        (console_term.cursor_enabled ? MODE0_CURSOR_ENABLED : 0) |
                        (d->cursor_lit ? MODE0_CURSOR_LIT : 0);
    mode0_regs.cursor_color = console_palette[CONSOLE_COLOR_CURSOR];
    mode0_regs.screen = d->cell_blink_phase |
                        (console_term.reverse_screen ? MODE0_SCREEN_REVERSE : 0);
    console_smooth_t *sm = &console_smoothing;
    const bool smooth = cells_smooth_shows(g);
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
    cells_published.smooth = smooth;
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

// Every byte goes to the terminal, which follows it at both sizes, up to
// one that has to wait for a smooth scroll still moving (term_holds), unless
// force. Returns how many went. A cursor's blink starts again when a byte
// moves it or changes how it looks: its style, or it coming back on.
static int cells_out_chars(const char *buf, int length, bool force)
{
    term_t *term = &console_term;
    const term_grid_t *const all[2] = {&console_40.grid, &console_80.grid};
    int i = 0;
    for (; i < length; i++)
    {
        if (!force && term_holds(term, (uint8_t)buf[i]))
            break;
        term_pos_t was[2];
        for (int n = 0; n < 2; n++)
            was[n] = *term_pos(all[n]);
        uint8_t ps = term->cursor_style;
        bool pe = term->cursor_enabled;
        term_write(term, (uint8_t)buf[i]);
        bool look = term->cursor_style != ps || (term->cursor_enabled && !pe);
        for (int n = 0; n < 2; n++)
        {
            const term_pos_t *now = term_pos(all[n]);
            if (look || now->x != was[n].x || now->y != was[n].y ||
                now->wrap_pending != was[n].wrap_pending)
                cells_cursor_restart_blink(cells_draw(all[n]));
        }
    }
    cells_publish();
    return i;
}

void cells_init(void)
{
    cells_restart();
    // become part of stdout
    manifold_set_term_out(cells_out_chars);
}

_Static_assert(MODE0_CELLS == CONSOLE_CELLS, "mode 0's cells are the console's");

/* The console as at power-up over mode 0's cells: it is the RESET hook,
 * because a load that fails leaves any value in all of them. */
void cells_restart(void)
{
    console_init(mode0_cells);
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
    if (!console_term.cursor_enabled)
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

// The words show a smooth scroll that has landed or was ended, so they are
// written again without it: the frame count wrapping round long after can
// never show it again. Asking puts away a record that has landed
// (console_smooth_grid), so it never shows again either.
static bool cells_smooth_task(void)
{
    const bool shows = cells_smooth_shows(console_shown(&console_term));
    return cells_published.smooth && !shows;
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
    cells_stale |= cells_smooth_task();
    cells_stale |= cells_blink_cursor(&draw_40);
    cells_stale |= cells_blink_cursor(&draw_80);
    cells_stale |= cells_cell_blink_phase_task(&draw_40);
    cells_stale |= cells_cell_blink_phase_task(&draw_80);
    // Another size or screen showing is published too, and a color changed.
    if (cells_stale || screen_moved || console_palette_changed ||
        cells_published.screen != console_shown(&console_term)->shown)
        cells_publish();
    cells_tidy(&console_40.grid);
    cells_tidy(&console_80.grid);
}
