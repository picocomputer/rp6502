/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/manifold.h"
#include "core/term/cells.h"
#include "core/term/color.h"
#include "core/term/term.h"
#include "core/vga/mode/mode0.h"
#include "core/vga/mode/mode.h"
#include "core/vga/pixel_format.h"
#include "core/vga/vga.h"
#include "machine.h"
#include <string.h>

// The console's screen on a machine that draws it: one terminal (term.c)
// followed at 40 and 80 columns, the cells it changes through the
// cells_ functions, the colors, the blinks, and the words mode 0 draws
// the size showing from.
//
// Designed to keep up with 115200 bps without flow control.
// The logic herein will make more sense if you remember this:

// 1. The screen data doesn't move when scrolling. Instead, the rows
//    are remapped through y_offset and row_idx, and mode 0's row words
//    say where each row's cells are.
// 2. The screen doesn't fully clear immediately. To keep the UART
//    buffer from overflowing, lines are cleared in a background task
//    and checked as the cursor moves into them. Nothing fills more
//    than one row of cells in a single operation.
// 3. Each cell's `attributes` byte holds render-time flags
//    (underline, blink, DEC graphics, etc.). The renderer's hot
//    path branches once on `attr != 0` per cell; the common path
//    pays only a 1-byte load and a predicted-not-taken branch.
// 4. A cell names its colors: a palette entry or a default color as an
//    entry of mode 0's palette, an RGB color as the pixel. The pen chooses
//    which as each cell is written, and a palette or default color that
//    changes afterwards is one write to the palette, which every cell
//    drawn with it shows on the next frame.

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

// Cursor blink half-period: normal cell vs a pending wrap, which blinks at 2x
// rate so it's still noticeable.
#define CELLS_CURSOR_BLINK_FRAMES 30
#define CELLS_CURSOR_BLINK_FAST_FRAMES 15

// Whenever input changes the cursor's logical position or its
// appearance (DECSCUSR style, DECTCEM visibility), the cursor is
// forced unlit and its next blink tick is armed this far out. Pure
// SGR/OSC sequences and color-only changes (OSC 12/112) leave the
// blink schedule untouched. The gap re-lights to the appropriate
// steady/blink state when it fires -- one frame, which is as soon as
// anything can be seen.
#define CELLS_CURSOR_INPUT_GAP_FRAMES 1

// Runs of cells. A cell is two words, and each has a byte of extras beside
// it (mode0.h), which goes wherever the cell goes. Everything that writes
// or moves more than a cell does it here.

static inline uint32_t cells_at(const mode0_cell_t *cell)
{
    return (uint32_t)(cell - mode0_cells);
}

// GCC turns a loop that copies or fills into a call of memmove or memset,
// and the C library's are slower here: its memmove takes 646 cycles to move
// a row of 80 a cell toward lower addresses and 1,103 toward higher, a byte
// at a time, where the loops below take 514 and 458.
#if defined(__GNUC__) && !defined(__clang__)
#define CELLS_RUN __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define CELLS_RUN
#endif

// The n cells at dst, and their extras, become `cell` and `extra`. Four
// cells a pass: 280 cycles for a row of 80, where a cell a pass takes 533.
static CELLS_RUN void cells_run_fill(mode0_cell_t *dst, unsigned n, mode0_cell_t cell,
                                     uint8_t extra)
{
    uint32_t w[2];
    memcpy(w, &cell, sizeof(w));
    const uint32_t w0 = w[0], w1 = w[1];
    mode_word_t *d = (mode_word_t *)dst;
    unsigned i = n;
    for (; i >= 4; i -= 4, d += 8)
    {
        d[0] = w0;
        d[1] = w1;
        d[2] = w0;
        d[3] = w1;
        d[4] = w0;
        d[5] = w1;
        d[6] = w0;
        d[7] = w1;
    }
    for (; i; i--, d += 2)
    {
        d[0] = w0;
        d[1] = w1;
    }
    memset(&mode0_extras[cells_at(dst)], extra, n);
}

// The n cells at src, and their extras, move to dst. The two may overlap,
// so a move toward lower addresses goes up from the first cell and one
// toward higher addresses goes down from the last. Four cells a pass, and
// four extras.
static CELLS_RUN void cells_run_move(mode0_cell_t *dst, const mode0_cell_t *src, unsigned n)
{
    mode_word_t *d = (mode_word_t *)dst;
    const mode_word_t *f = (const mode_word_t *)src;
    uint8_t *xd = &mode0_extras[cells_at(dst)];
    const uint8_t *xf = &mode0_extras[cells_at(src)];
    unsigned w = 2 * n, x = n;
    if (dst < src)
    {
        for (; w >= 8; w -= 8, d += 8, f += 8)
        {
            const uint32_t a = f[0], b = f[1], c = f[2], e = f[3];
            const uint32_t g = f[4], h = f[5], i = f[6], j = f[7];
            d[0] = a;
            d[1] = b;
            d[2] = c;
            d[3] = e;
            d[4] = g;
            d[5] = h;
            d[6] = i;
            d[7] = j;
        }
        for (; w; w--)
            *d++ = *f++;
        for (; x >= 4; x -= 4, xd += 4, xf += 4)
        {
            uint32_t v;
            memcpy(&v, xf, 4);
            memcpy(xd, &v, 4);
        }
        for (; x; x--)
            *xd++ = *xf++;
    }
    else if (dst > src)
    {
        d += w;
        f += w;
        for (; w >= 8; w -= 8)
        {
            d -= 8;
            f -= 8;
            const uint32_t a = f[7], b = f[6], c = f[5], e = f[4];
            const uint32_t g = f[3], h = f[2], i = f[1], j = f[0];
            d[7] = a;
            d[6] = b;
            d[5] = c;
            d[4] = e;
            d[3] = g;
            d[2] = h;
            d[1] = i;
            d[0] = j;
        }
        for (; w; w--)
            *--d = *--f;
        xd += x;
        xf += x;
        for (; x >= 4; x -= 4)
        {
            uint32_t v;
            xd -= 4;
            xf -= 4;
            memcpy(&v, xf, 4);
            memcpy(xd, &v, 4);
        }
        for (; x; x--)
            *--xd = *--xf;
    }
}

// A lazy erase owed to a row: its glyph, a space or DECALN's E, its colors,
// and whether it leaves the protected cells alone.
typedef struct
{
    uint16_t fg, bg; /* as a cell holds them */
    uint8_t glyph;
    bool selective;
} cells_erase_t;

// The cells of a screen and how its rows are laid out: the scroll remap and
// the lazy erases still owed. Rows are indexed by their place on the screen,
// except row_idx, which is indexed by slot.
typedef struct
{
    mode0_cell_t *mem;
    uint8_t row_idx[TERM_MAX_HEIGHT];
    bool dirty[TERM_MAX_HEIGHT];
    cells_erase_t erase[TERM_MAX_HEIGHT];
    uint8_t y_offset;
    /* The rows scrolled off the top that are still in the cell store, the
     * newest first. A taller view shows them again; ED 3 forgets them. */
    uint8_t hidden;
    bool all_clean;
} screen_buf_t;

// The terminal at one size and its screens' cells.
typedef struct
{
    /* First, so the cells find their own from the grid they are handed. */
    term_grid_t grid;
    uint8_t lines[2 * TERM_MAX_HEIGHT];
    screen_buf_t bufs[2]; /* [0] = primary, [1] = alt, as the terminal's screens */
    unsigned long cursor_frame;
    bool cursor_lit;
    unsigned long cell_blink_frame;
    uint8_t cell_blink_phase;
} cells_t;

term_t cells_term;
static cells_t cells_40;
static cells_t cells_80;
/* Something mode 0 draws changed outside a batch of the console's bytes,
 * for cells_task to publish. */
static bool cells_stale;
static uint16_t cells_cursor_color; // OSC 12

/* A grid of this file's, or NULL for one with no cells: a program's own size
 * (term_add_grid), which the screen does not draw. */
static inline cells_t *cells_of(term_grid_t *g)
{
    if (g == &cells_40.grid || g == &cells_80.grid)
        return (cells_t *)g;
    return NULL;
}

// The screen showing, which the terminal chooses.
static inline screen_buf_t *cells_buf(cells_t *cells)
{
    return &cells->bufs[cells_term.alt_active ? 1 : 0];
}

// Add `row` to `y_offset` and wrap into [0, TERM_MAX_HEIGHT). Used to
// translate a logical row into a physical row_idx[] slot. Inputs are
// constrained to [0, TERM_MAX_HEIGHT), so the sum can't exceed 2*MAX-1
// (=63), which fits in uint8_t.
static inline uint8_t cells_buf_slot(uint8_t y_offset, uint8_t row)
{
    uint8_t slot = (uint8_t)(y_offset + row);
    if (slot >= TERM_MAX_HEIGHT)
        slot -= TERM_MAX_HEIGHT;
    return slot;
}

static inline mode0_cell_t *cells_buf_row(const screen_buf_t *buf, uint8_t width, uint8_t y)
{
    uint8_t slot = cells_buf_slot(buf->y_offset, y);
    return buf->mem + (uint32_t)buf->row_idx[slot] * width;
}

// Translate a logical row (0..height-1) into the start of its physical
// cell row.
static inline mode0_cell_t *cells_row_ptr(cells_t *cells, uint8_t y)
{
    return cells_buf_row(cells_buf(cells), cells->grid.width, y);
}

/* ---- colors ------------------------------------------------------------ */

// A pixel at half brightness, for SGR 2.
static uint16_t cells_faint(uint16_t c)
{
    uint16_t r = SCANVIDEO_R5_FROM_PIXEL(c) >> 1;
    uint16_t g = SCANVIDEO_G5_FROM_PIXEL(c) >> 1;
    uint16_t b = SCANVIDEO_B5_FROM_PIXEL(c) >> 1;
    return SCANVIDEO_PIXEL_FROM_RGB5(r, g, b) | (c & SCANVIDEO_ALPHA_MASK);
}

// A cell's color at half brightness: an RGB pixel halved, or the faint twin
// of an entry.
static uint16_t cells_color_faint(uint16_t color)
{
    if (color & SCANVIDEO_ALPHA_MASK)
        return cells_faint(color);
    return (uint16_t)(color + MODE0_ENTRY(MODE0_ENTRIES, false));
}

// A pen color before bold, faint and iCE colors, as a cell holds it: the
// palette entry, the RGB pixel, or the default (dflt, MODE0_DEFAULT_FG or
// _BG).
static uint16_t cells_base(const term_color_t *c, unsigned dflt)
{
    switch (c->kind)
    {
    case TERM_COLOR_INDEX:
        return MODE0_ENTRY(c->index, false);
    case TERM_COLOR_RGB:
        return c->rgb | SCANVIDEO_ALPHA_MASK;
    default:
        return MODE0_ENTRY(dflt, false);
    }
}

// The colors the pen draws in. Bold brightens palette colors 0-7 and the
// default foreground (to 15, as Linux does); faint halves each channel. In
// iCE colors mode (?33) SGR 5/6 brightens a background of 0-7 or the default
// instead of blinking -- the IBM-VGA hack the BBS scene relies on. A
// brightened color is the entry it was brightened to.
static uint16_t cells_pen_fg(const term_pen_t *pen)
{
    uint16_t fg;
    if (pen->bold && pen->fg.kind == TERM_COLOR_INDEX && pen->fg.index < 8)
        fg = MODE0_ENTRY(pen->fg.index + 8, false);
    else if (pen->bold && pen->fg.kind == TERM_COLOR_DEFAULT)
        fg = MODE0_ENTRY(15, false);
    else
        fg = cells_base(&pen->fg, MODE0_DEFAULT_FG);
    return pen->faint ? cells_color_faint(fg) : fg;
}

static uint16_t cells_pen_bg(const term_t *term, const term_pen_t *pen)
{
    bool bright = term->ice_colors && (pen->sgr_attr & TERM_ATTR_ANY_BLINK);
    if (bright && pen->bg.kind == TERM_COLOR_INDEX && pen->bg.index < 8)
        return MODE0_ENTRY(pen->bg.index + 8, false);
    if (bright && pen->bg.kind == TERM_COLOR_DEFAULT)
        return MODE0_ENTRY(8, false);
    return cells_base(&pen->bg, MODE0_DEFAULT_BG);
}

/* ---- cells and rows ---------------------------------------------------- */

// Paint [start, end) of `row` with erased cells: the erase's glyph in its
// colors, with no attributes and no guard. A selective erase leaves the
// guarded cells as they are. An erase takes the pen's colors, not reversed,
// and none of its attributes, as xterm and Linux do. The foreground matters
// too, because the cursor is drawn from the cell it sits on.
static void fill_glyph(mode0_cell_t *row, unsigned start, unsigned end,
                       const cells_erase_t *e)
{
    const mode0_cell_t blank = {e->glyph, 0, e->fg, e->bg, e->fg};
    if (!e->selective)
    {
        cells_run_fill(row + start, end - start, blank, 0);
        return;
    }
    uint8_t *extra = &mode0_extras[cells_at(row)];
    for (unsigned i = start; i < end; i++)
        if (!(extra[i] & MODE0_EXTRA_GUARD))
        {
            row[i] = blank;
            extra[i] = 0;
        }
}

// An erase in the pen's colors, selective or not.
static cells_erase_t cells_pen_erase(const term_t *term, bool selective)
{
    const term_pen_t *pen = term_pen(term);
    cells_erase_t e = {cells_pen_fg(pen), cells_pen_bg(term, pen), ' ', selective};
    return e;
}

// Erase [start, end) of `row` in the pen's colors.
static void cells_fill_erased(term_t *term, mode0_cell_t *row, unsigned start, unsigned end,
                              bool selective)
{
    const cells_erase_t e = cells_pen_erase(term, selective);
    fill_glyph(row, start, end, &e);
}

// Make sure you call this any time you change rows.
// It will process any pending screen clears on the row.
static void cells_clean_line(cells_t *cells, uint8_t y)
{
    screen_buf_t *buf = cells_buf(cells);
    if (!buf->dirty[y])
        return;
    buf->dirty[y] = false;
    fill_glyph(cells_row_ptr(cells, y), 0, cells->grid.width, &buf->erase[y]);
}

// Force the cursor unlit and arm the input-gap timer. Used by movement
// and by appearance changes (DECSCUSR, DECTCEM enable) so the cursor's new
// state lands on a clean blink boundary.
static inline void cells_cursor_restart_blink(cells_t *cells)
{
    cells->cursor_lit = false;
    cells->cursor_frame = vga_frame_count() + CELLS_CURSOR_INPUT_GAP_FRAMES;
}

// Restart the cursor blink and the blinking text together, both lit, so a
// reset leaves them in step.
static void cells_restart_blinks(cells_t *cells)
{
    cells_cursor_restart_blink(cells);
    cells->cell_blink_phase = 0;
    cells->cell_blink_frame = vga_frame_count() + TERM_BLINK_TICK_FRAMES;
}

// Clean one dirty row in `buf`. Returns true if a row was cleaned this
// call, false if the buffer was already fully clean. Operates on an
// explicit buffer so the task path can drain the inactive (alt) buffer
// too -- otherwise the lazy mark on RIS would leave alt dirty until ?47
// swaps it in.
static bool cells_clean_one_dirty(screen_buf_t *buf, uint8_t width, uint8_t height)
{
    if (buf->all_clean)
        return false;
    for (uint8_t i = 0; i < height; i++)
    {
        if (buf->dirty[i])
        {
            buf->dirty[i] = false;
            fill_glyph(cells_buf_row(buf, width, i), 0, width, &buf->erase[i]);
            return true;
        }
    }
    buf->all_clean = true;
    return false;
}

// Clean at most one row per tick: prefer the active buffer; only touch
// the inactive one if the active is already clean. Invariant: the cursor's
// row is never dirty on the active buffer (the terminal touches every row it
// puts the cursor on), so the cursor cell is never affected by this task and
// it never has to call cells_cursor_restart_blink.
static void cells_clean_task(cells_t *cells)
{
    if (cells_clean_one_dirty(cells_buf(cells), cells->grid.width, cells->grid.height))
        return;
    screen_buf_t *other = &cells->bufs[cells_term.alt_active ? 0 : 1];
    if (other->mem)
        cells_clean_one_dirty(other, cells->grid.width, cells->grid.height);
}

static void cells_shift_meta_up(screen_buf_t *s, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++)
    {
        s->dirty[i] = s->dirty[i + 1];
        s->erase[i] = s->erase[i + 1];
    }
}

static void cells_shift_meta_down(screen_buf_t *s, uint8_t start)
{
    for (int i = start; i > 0; i--)
    {
        s->dirty[i] = s->dirty[i - 1];
        s->erase[i] = s->erase[i - 1];
    }
}

// Lazy-erase rows [from, to). A selective erase of a row already owed a
// whole one stays whole: that erase leaves no protected cell for this one
// to spare.
static void cells_mark_rows_with(screen_buf_t *s, uint8_t from, uint8_t to,
                                 const cells_erase_t *e)
{
    for (uint8_t i = from; i < to; i++)
    {
        bool whole = s->dirty[i] && !s->erase[i].selective;
        s->dirty[i] = true;
        s->erase[i] = *e;
        if (whole)
            s->erase[i].selective = false;
    }
    s->all_clean = false;
}

// Lazy-erase rows [from, to) to spaces in the default colors.
static void cells_mark_rows_default(screen_buf_t *s, uint8_t from, uint8_t to)
{
    const cells_erase_t e = {MODE0_ENTRY(MODE0_DEFAULT_FG, false),
                             MODE0_ENTRY(MODE0_DEFAULT_BG, false), ' ', false};
    cells_mark_rows_with(s, from, to, &e);
}

// Lazy-erase rows [from, to) of the active screen in the pen's colors.
static void cells_mark_rows_erase(term_t *term, cells_t *cells, uint8_t from, uint8_t to,
                                  bool selective)
{
    const cells_erase_t e = cells_pen_erase(term, selective);
    cells_mark_rows_with(cells_buf(cells), from, to, &e);
}

// Lazy clear of the given screen buffer: resets y_offset / row_idx to
// identity and marks every visible row dirty with the given erase.
// No cell memory is touched here -- cells_clean_task drains dirty rows in
// the background (one row per tick, on both buffers), and the active
// buffer's cursor row is also cleaned synchronously when the terminal
// touches it.
static void cells_mark_buf_erased(screen_buf_t *buf, uint8_t height, const cells_erase_t *e)
{
    buf->y_offset = 0;
    buf->hidden = 0;
    for (uint8_t y = 0; y < TERM_MAX_HEIGHT; y++)
    {
        buf->row_idx[y] = y;
        buf->dirty[y] = false;
    }
    cells_mark_rows_with(buf, 0, height, e);
}

/* ---- what the terminal asks of the cells ------------------------------- */

void cells_touch(term_t *term, term_grid_t *g, uint8_t y)
{
    if (!cells_of(g))
        return;
    (void)term;
    cells_clean_line(cells_of(g), y);
}

// The cell a glyph lands as: the pen's colors and attributes, through the
// active character set. DEC Special Graphics is applied at render time, not
// at the font_code level: the cell keeps the original byte and is flagged so
// the renderer pulls from font_dec_*. The UK set's pound sign is the DEC set's
// 0x7D. The 0x80..0xFF font range stays reserved for the active codepage.
// SGR 7 and 8 swap and hide at the cell, and in iCE colors mode (?33) the
// blink bits are left off the cell: the bright background is the blink. An
// underline SGR 58 did not color holds the foreground's color.
static mode0_cell_t cells_glyph_cell(term_t *term, uint8_t byte)
{
    const term_pen_t *pen = term_pen(term);
    mode0_cell_t cell;
    uint16_t f = cells_pen_fg(pen);
    uint16_t b = cells_pen_bg(term, pen);
    if (pen->reverse)
    {
        uint16_t tmp = f;
        f = b;
        b = tmp;
    }
    if (pen->conceal)
        f = b;
    cell.fg_color = f;
    cell.bg_color = b;
    uint8_t cell_mask = TERM_ATTR_RENDER_MASK & ~TERM_ATTR_DEC;
    if (term->ice_colors)
        cell_mask &= (uint8_t)~TERM_ATTR_ANY_BLINK;
    cell.attributes = (uint8_t)(pen->sgr_attr & cell_mask);
    uint8_t set = pen->gl_is_g1 ? pen->g1_charset : pen->g0_charset;
    if (set == TERM_CS_DEC && byte >= 0x5F && byte <= 0x7E)
        cell.attributes |= TERM_ATTR_DEC;
    else if (set == TERM_CS_UK && byte == '#')
    {
        byte = 0x7D;
        cell.attributes |= TERM_ATTR_DEC;
    }
    cell.font_code = byte;
    cell.ul_color = pen->ul_set ? cells_base(&pen->ul, MODE0_DEFAULT_FG) : f;
    return cell;
}

void cells_glyphs(term_t *term, term_grid_t *g, uint8_t y, uint8_t x, uint8_t byte, unsigned n)
{
    if (!cells_of(g))
        return;
    mode0_cell_t cell = cells_glyph_cell(term, byte);
    cells_run_fill(cells_row_ptr(cells_of(g), y) + x, n, cell,
                   term_protecting(term) ? MODE0_EXTRA_GUARD : 0);
}

void cells_erase(term_t *term, term_grid_t *g, uint8_t y, unsigned from, unsigned to,
                 bool selective)
{
    if (!cells_of(g))
        return;
    cells_fill_erased(term, cells_row_ptr(cells_of(g), y), from, to, selective);
}

void cells_insert(term_t *term, term_grid_t *g, uint8_t y, uint8_t x, unsigned n)
{
    if (!cells_of(g))
        return;
    unsigned avail = term_cols(g, y) - x;
    mode0_cell_t *row = cells_row_ptr(cells_of(g), y);
    cells_run_move(&row[x + n], &row[x], avail - n);
    cells_fill_erased(term, row, x, x + n, false);
}

void cells_delete(term_t *term, term_grid_t *g, uint8_t y, uint8_t x, unsigned n)
{
    if (!cells_of(g))
        return;
    unsigned cols = term_cols(g, y);
    unsigned avail = cols - x;
    mode0_cell_t *row = cells_row_ptr(cells_of(g), y);
    cells_run_move(&row[x], &row[x + n], avail - n);
    cells_fill_erased(term, row, cols - n, cols, false);
}

// A line waiting on a whole lazy erase is left to it. One waiting on a
// selective erase is erased first, because its protected cells move.
void cells_shift(term_t *term, term_grid_t *g, uint8_t top, uint8_t bot, unsigned count, bool left)
{
    if (!cells_of(g))
        return;
    cells_t *cells = cells_of(g);
    for (uint8_t y = top; y <= bot; y++)
    {
        const screen_buf_t *buf = cells_buf(cells);
        if (buf->dirty[y] && !buf->erase[y].selective)
            continue;
        cells_clean_line(cells, y);
        unsigned cols = term_cols(g, y);
        unsigned n = count < cols ? count : cols;
        mode0_cell_t *row = cells_row_ptr(cells, y);
        if (left)
        {
            cells_run_move(row, row + n, cols - n);
            cells_fill_erased(term, row, cols - n, cols, false);
        }
        else
        {
            cells_run_move(row + n, row, cols - n);
            cells_fill_erased(term, row, 0, n, false);
        }
    }
}

void cells_erase_rows(term_t *term, term_grid_t *g, uint8_t from, uint8_t to,
                      bool selective)
{
    if (!cells_of(g))
        return;
    cells_mark_rows_erase(term, cells_of(g), from, to, selective);
}

void cells_align(term_t *term, term_grid_t *g)
{
    if (!cells_of(g))
        return;
    (void)term;
    const cells_erase_t e = {MODE0_ENTRY(MODE0_DEFAULT_FG, false),
                             MODE0_ENTRY(MODE0_DEFAULT_BG, false), 'E', false};
    cells_mark_rows_with(cells_buf(cells_of(g)), 0, g->height, &e);
}

// Rotate row_idx within the region by n, and each row's lazy erase with it;
// the n rows coming in are lazy-erased in the pen's colors.
void cells_scroll(term_t *term, term_grid_t *g, uint8_t top, uint8_t bot, uint8_t n, bool up)
{
    if (!cells_of(g))
        return;
    cells_t *cells = cells_of(g);
    screen_buf_t *s = cells_buf(cells);
    uint8_t y_off = s->y_offset;
    uint8_t saved[TERM_MAX_HEIGHT];
    if (up)
    {
        for (uint8_t i = 0; i < n; i++)
            saved[i] = s->row_idx[cells_buf_slot(y_off, (uint8_t)(top + i))];
        for (uint8_t i = top; (uint8_t)(i + n) <= bot; i++)
            s->row_idx[cells_buf_slot(y_off, i)] = s->row_idx[cells_buf_slot(y_off, (uint8_t)(i + n))];
        for (uint8_t i = 0; i < n; i++)
            s->row_idx[cells_buf_slot(y_off, (uint8_t)(bot - n + 1 + i))] = saved[i];
        for (uint8_t i = top; (uint8_t)(i + n) <= bot; i++)
        {
            s->dirty[i] = s->dirty[i + n];
            s->erase[i] = s->erase[i + n];
        }
        cells_mark_rows_erase(term, cells, (uint8_t)(bot - n + 1), (uint8_t)(bot + 1), false);
    }
    else
    {
        for (uint8_t i = 0; i < n; i++)
            saved[i] = s->row_idx[cells_buf_slot(y_off, (uint8_t)(bot - n + 1 + i))];
        for (int i = (int)bot; i - (int)n >= (int)top; i--)
            s->row_idx[cells_buf_slot(y_off, (uint8_t)i)] = s->row_idx[cells_buf_slot(y_off, (uint8_t)(i - n))];
        for (uint8_t i = 0; i < n; i++)
            s->row_idx[cells_buf_slot(y_off, (uint8_t)(top + i))] = saved[i];
        for (int i = (int)bot; i - (int)n >= (int)top; i--)
        {
            s->dirty[i] = s->dirty[i - n];
            s->erase[i] = s->erase[i - n];
        }
        cells_mark_rows_erase(term, cells, top, (uint8_t)(top + n), false);
    }
}

// The fast scroll: the viewport rotates through y_offset.
void cells_index(term_t *term, term_grid_t *g, bool up)
{
    if (!cells_of(g))
        return;
    cells_t *cells = cells_of(g);
    screen_buf_t *s = cells_buf(cells);
    uint8_t last = (uint8_t)(g->height - 1);
    if (up)
    {
        if (++s->y_offset == TERM_MAX_HEIGHT)
            s->y_offset = 0;
        if (s->hidden < TERM_MAX_HEIGHT - g->height)
            s->hidden++;
        cells_shift_meta_up(s, last);
        cells_mark_rows_erase(term, cells, last, g->height, false);
    }
    else
    {
        s->y_offset = s->y_offset ? (uint8_t)(s->y_offset - 1) : (uint8_t)(TERM_MAX_HEIGHT - 1);
        if (s->hidden)
            s->hidden--;
        cells_shift_meta_down(s, last);
        cells_mark_rows_erase(term, cells, 0, 1, false);
    }
}

void cells_forget(term_t *term, term_grid_t *g)
{
    if (!cells_of(g))
        return;
    (void)term;
    cells_buf(cells_of(g))->hidden = 0;
}

void cells_clear(term_t *term, term_grid_t *g)
{
    if (!cells_of(g))
        return;
    const cells_erase_t e = cells_pen_erase(term, false);
    cells_mark_buf_erased(cells_buf(cells_of(g)), g->height, &e);
}

void cells_height(term_grid_t *g, bool grow, bool top)
{
    if (!cells_of(g))
        return;
    cells_t *cells = cells_of(g);
    screen_buf_t *s = cells_buf(cells);
    uint8_t h = g->height;
    if (grow && top)
    {
        // Everything moves down a row and the row above comes into view. It
        // is a row of scrollback when there is one, otherwise a blank one.
        if (!s->y_offset)
            s->y_offset = TERM_MAX_HEIGHT - 1;
        else
            s->y_offset--;
        cells_shift_meta_down(s, (uint8_t)(h - 1));
        if (s->hidden)
        {
            s->hidden--;
            s->dirty[0] = false;
        }
        else
            cells_mark_rows_default(s, 0, 1);
        // shift_meta_down may have copied a dirty flag into the cursor's new
        // row; uphold the "the cursor's row is never dirty" invariant.
        cells_clean_line(cells, term_pos(g)->y);
    }
    else if (grow)
        // Expose one fresh row at the bottom. The cleaner task drains it in
        // the background; the worst case is one row of stale content for a
        // frame or two.
        cells_mark_rows_default(s, (uint8_t)(h - 1), h);
    else if (top)
    {
        // The top row goes out of view and joins the scrollback.
        if (++s->y_offset >= TERM_MAX_HEIGHT)
            s->y_offset -= TERM_MAX_HEIGHT;
        cells_shift_meta_up(s, h);
        s->hidden++;
        cells_clean_line(cells, term_pos(g)->y);
    }
    // Otherwise the bottom row is no longer visible; the cleaner only drains
    // visible rows. Leave it -- a later grow that re-exposes the row will
    // mark it dirty.

    // The rows above the view and the rows below it share the ring, so the
    // scrollback is at most what the view leaves over.
    uint8_t room = (uint8_t)(TERM_MAX_HEIGHT - h);
    for (int b = 0; b < 2; b++)
        if (cells->bufs[b].hidden > room)
            cells->bufs[b].hidden = room;
}

/* ---- the palette --------------------------------------------------------
 *
 * A palette entry or default color that changes reaches every cell drawn
 * with it, as on xterm and Linux, because those cells hold the entry and
 * not the color. */

// Set a palette entry or default color, and its faint twin.
static void cells_set_color(unsigned entry, uint16_t color)
{
    mode0_palette[entry] = color;
    mode0_palette[MODE0_ENTRIES + entry] = cells_faint(color);
}

void cells_colors_reset(term_t *term)
{
    (void)term;
    for (unsigned i = 0; i < 256; i++)
        cells_set_color(i, color_256[i]);
    cells_set_color(MODE0_DEFAULT_FG, color_256[CELLS_FG_COLOR_INDEX]);
    cells_set_color(MODE0_DEFAULT_BG, color_256[CELLS_BG_COLOR_INDEX]);
    cells_cursor_color = color_256[CELLS_FG_COLOR_INDEX];
}

void cells_reset(term_t *term, term_grid_t *g)
{
    if (!cells_of(g))
        return;
    (void)term;
    cells_t *cells = cells_of(g);
    if (cells->bufs[1].mem)
    {
        const cells_erase_t e = {MODE0_ENTRY(MODE0_DEFAULT_FG, false),
                                 MODE0_ENTRY(MODE0_DEFAULT_BG, false), ' ', false};
        cells_mark_buf_erased(&cells->bufs[1], g->height, &e);
    }
    cells_restart_blinks(cells);
}

void cells_color(term_t *term, unsigned which, uint16_t color)
{
    (void)term;
    if (which < 256)
        cells_set_color(which, color);
    else if (which == CELLS_COLOR_FG)
        cells_set_color(MODE0_DEFAULT_FG, color);
    else if (which == CELLS_COLOR_BG)
        cells_set_color(MODE0_DEFAULT_BG, color);
    else if (which == CELLS_COLOR_CURSOR)
        cells_cursor_color = color;
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

void cells_publish(void)
{
    cells_t *cells = cells_visible();
    const screen_buf_t *buf = cells_buf(cells);
    const uint8_t *ls = term_lines(&cells->grid);
    for (uint8_t y = 0; y < cells->grid.height; y++)
        mode0_regs.row[y] = MODE0_ROW(cells_buf_row(buf, cells->grid.width, y) - mode0_cells,
                                      ls[y]);
    const term_pos_t *pos = term_pos(&cells->grid);
    // A pending wrap is shown as the column past the last, which is how the
    // renderer knows to draw it.
    mode0_regs.cursor = MODE0_CURSOR(pos->x + (pos->wrap_pending ? 1 : 0),
                                     pos->y, cells_term.cursor_style) |
                        (cells_term.cursor_enabled ? MODE0_CURSOR_ENABLED : 0) |
                        (cells->cursor_lit ? MODE0_CURSOR_LIT : 0);
    mode0_regs.cursor_color = cells_cursor_color;
    mode0_regs.screen = cells->cell_blink_phase |
                        (cells_term.reverse_screen ? MODE0_SCREEN_REVERSE : 0);
    cells_stale = false;
}

void cells_reply(const term_t *term, const char *s, size_t n)
{
    (void)term;
    manifold_reply(s, n);
}

term_grid_t *cells_shown(const term_t *term)
{
    (void)term;
    return &cells_visible()->grid;
}

// Every byte goes to the terminal, which follows it at both sizes. A cursor's
// blink starts again when a byte moves it or changes how it looks: its
// style, or it coming back on.
static void cells_out_chars(const char *buf, int length)
{
    term_t *term = &cells_term;
    cells_t *all[2] = {&cells_40, &cells_80};
    for (int i = 0; i < length; i++)
    {
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
                cells_cursor_restart_blink(all[n]);
        }
    }
    cells_publish();
}

void cells_init(void)
{
    // The colors start as the reset leaves them.
    cells_colors_reset(&cells_term);
    // The cells are mode 0's: each size's primary, then its alternate.
    mode0_cell_t *mem = mode0_cells;
    cells_40.bufs[0].mem = mem;
    mem += 40 * TERM_MAX_HEIGHT;
#if TERM_ALT_SCREEN
    cells_40.bufs[1].mem = mem;
    mem += 40 * TERM_MAX_HEIGHT;
#endif
    cells_80.bufs[0].mem = mem;
#if TERM_ALT_SCREEN
    mem += 80 * TERM_MAX_HEIGHT;
    cells_80.bufs[1].mem = mem;
#endif
    /* Without an alt store, bufs[1] never has cells, and the terminal never
     * switches to it. */
    term_grid_init(&cells_40.grid, 40, cells_40.lines, TERM_MAX_HEIGHT);
    term_grid_init(&cells_80.grid, 80, cells_80.lines, TERM_MAX_HEIGHT);
    term_grid_t *const grids[] = {&cells_40.grid, &cells_80.grid};
    term_init(&cells_term, grids, 2);
    // become part of stdout
    manifold_set_term_out(cells_out_chars);
}

// Cursor blink timing only — toggles cursor_lit on the configured cadence
// (fast while a wrap is pending, normal otherwise) and forces unlit when
// the cursor is disabled. Steady-vs-blink handling lives in the renderers,
// which ignore cursor_lit when cursor_style is steady; this keeps the
// timing path style-agnostic.
// True when cursor_lit changed.
static bool cells_blink_cursor(cells_t *cells)
{
    bool was = cells->cursor_lit;
    if (!cells_term.cursor_enabled)
        cells->cursor_lit = false;
    else if ((long)(vga_frame_count() - cells->cursor_frame) >= 0)
    {
        cells->cursor_lit = !cells->cursor_lit;
        cells->cursor_frame = vga_frame_count() + (term_pos(&cells->grid)->wrap_pending
                                                       ? CELLS_CURSOR_BLINK_FAST_FRAMES
                                                       : CELLS_CURSOR_BLINK_FRAMES);
    }
    return cells->cursor_lit != was;
}

// SGR-5/6 cell blink phase, per size: a free-running 2-bit counter at bits 0-1
// (TERM_ATTR_ANY_BLINK). Bit 0 (TERM_ATTR_BLINK_FAST) flips every tick -> SGR 6 rapid;
// bit 1 (TERM_ATTR_BLINK) every two ticks -> SGR 5 slow. The renderer ANDs a cell's
// single blink bit against this value, so each cell darkens only on its own
// rate's off-phase, and both relight together at phase 0. Per size so the
// cursor timer and cell blink timer share an owner instead of crossing a
// global. True when the phase changed.
static bool cells_cell_blink_phase_task(cells_t *cells)
{
    if ((long)(vga_frame_count() - cells->cell_blink_frame) < 0)
        return false;
    cells->cell_blink_phase =
        (uint8_t)((cells->cell_blink_phase + 1) & TERM_ATTR_ANY_BLINK);
    cells->cell_blink_frame = vga_frame_count() + TERM_BLINK_TICK_FRAMES;
    return true;
}

void cells_task(void)
{
    cells_stale |= cells_blink_cursor(&cells_40);
    cells_stale |= cells_blink_cursor(&cells_80);
    cells_stale |= cells_cell_blink_phase_task(&cells_40);
    cells_stale |= cells_cell_blink_phase_task(&cells_80);
    if (cells_stale)
        cells_publish();
    cells_clean_task(&cells_40);
    cells_clean_task(&cells_80);
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

/* ---- savestate ----------------------------------------------------------
 *
 * The terminal, then for each size its grid, the layout of each screen's
 * cells and the blinks, then the colors, the cells and the palette. Written
 * field by field, so no padding reaches the blob. */

// A color a blob gives a cell names an entry the palette has.
static inline bool cells_color_ok(uint16_t color)
{
    return (color & SCANVIDEO_ALPHA_MASK) || (color >> MODE0_ENTRY_SHIFT) < 2 * MODE0_ENTRIES;
}

static void cells_put_buf(sst_cursor_t *c, const screen_buf_t *b)
{
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        sst_put_u8(c, b->row_idx[i]);
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        sst_put_bool(c, b->dirty[i]);
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
    {
        const cells_erase_t *e = &b->erase[i];
        sst_put_u16(c, e->fg);
        sst_put_u16(c, e->bg);
        sst_put_u8(c, e->glyph);
        sst_put_bool(c, e->selective);
    }
    sst_put_u8(c, b->y_offset);
    sst_put_u8(c, b->hidden);
    sst_put_bool(c, b->all_clean);
}

static bool cells_get_buf(sst_cursor_t *c, screen_buf_t *b)
{
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        b->row_idx[i] = sst_get_u8(c);
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        b->dirty[i] = sst_get_bool(c);
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
    {
        cells_erase_t *e = &b->erase[i];
        e->fg = sst_get_u16(c);
        e->bg = sst_get_u16(c);
        e->glyph = sst_get_u8(c);
        e->selective = sst_get_bool(c);
    }
    b->y_offset = sst_get_u8(c);
    b->hidden = sst_get_u8(c);
    b->all_clean = sst_get_bool(c);
    /* Everything here indexes the cell array. row_idx is the scroll remap and
     * y_offset is added to it before the lookup, so both are bounded. */
    if (!sst_ok(c) || b->y_offset >= TERM_MAX_HEIGHT || b->hidden >= TERM_MAX_HEIGHT)
        return false;
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        if (b->row_idx[i] >= TERM_MAX_HEIGHT ||
            !cells_color_ok(b->erase[i].fg) || !cells_color_ok(b->erase[i].bg))
            return false;
    return true;
}

static void cells_put_one(sst_cursor_t *c, const cells_t *cells)
{
    term_grid_sst_put(c, &cells->grid);
    cells_put_buf(c, &cells->bufs[0]);
    cells_put_buf(c, &cells->bufs[1]);
    sst_put_u64(c, cells->cursor_frame);
    sst_put_bool(c, cells->cursor_lit);
    sst_put_u64(c, cells->cell_blink_frame);
    sst_put_u8(c, cells->cell_blink_phase);
}

static bool cells_get_one(sst_cursor_t *c, cells_t *cells)
{
    uint8_t width = cells->grid.width;
    if (!term_grid_sst_get(c, &cells->grid) || cells->grid.width != width ||
        !cells_get_buf(c, &cells->bufs[0]) || !cells_get_buf(c, &cells->bufs[1]))
        return false;
    cells->cursor_frame = (unsigned long)sst_get_u64(c);
    cells->cursor_lit = sst_get_bool(c);
    cells->cell_blink_frame = (unsigned long)sst_get_u64(c);
    cells->cell_blink_phase = sst_get_u8(c);
    return sst_ok(c);
}

void cells_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u8(c, TERM_MAX_HEIGHT);
    sst_put_bool(c, TERM_ALT_SCREEN ? true : false);
    sst_put_u8(c, 2); /* how many sizes this machine has */
    term_sst_put(c, &cells_term);
    cells_put_one(c, &cells_40);
    cells_put_one(c, &cells_80);
    sst_put_u16(c, cells_cursor_color);
    for (int b = 0; b < (TERM_ALT_SCREEN ? 2 : 1); b++)
    {
        sst_put(c, cells_40.bufs[b].mem, 40 * TERM_MAX_HEIGHT * sizeof(mode0_cell_t));
        sst_put(c, cells_80.bufs[b].mem, 80 * TERM_MAX_HEIGHT * sizeof(mode0_cell_t));
    }
    sst_put(c, mode0_extras, sizeof mode0_extras);
    /* The faint half of the palette follows from the other. */
    for (unsigned i = 0; i < MODE0_ENTRIES; i++)
        sst_put_u16(c, mode0_palette[i]);
}

bool cells_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint8_t rows = sst_get_u8(c);
    bool alt = sst_get_bool(c);
    uint8_t count = sst_get_u8(c);
    if (!sst_ok(c) || rows != TERM_MAX_HEIGHT ||
        alt != (TERM_ALT_SCREEN ? true : false) || count != 2)
        return false;
    if (!term_sst_get(c, &cells_term) ||
        !cells_get_one(c, &cells_40) || !cells_get_one(c, &cells_80))
        return false;
    cells_cursor_color = sst_get_u16(c);
    /* Never mem: the cells are mode 0's and their addresses are already
     * right. Only what is in them is the blob's. */
    for (int b = 0; b < (TERM_ALT_SCREEN ? 2 : 1); b++)
    {
        sst_get(c, cells_40.bufs[b].mem, 40 * TERM_MAX_HEIGHT * sizeof(mode0_cell_t));
        sst_get(c, cells_80.bufs[b].mem, 80 * TERM_MAX_HEIGHT * sizeof(mode0_cell_t));
    }
    sst_get(c, mode0_extras, sizeof mode0_extras);
    for (unsigned i = 0; i < MODE0_ENTRIES; i++)
        cells_set_color(i, sst_get_u16(c));
    if (!sst_ok(c))
        return false;
    for (unsigned i = 0; i < MODE0_CELLS; i++)
        if (!cells_color_ok(mode0_cells[i].fg_color) || !cells_color_ok(mode0_cells[i].bg_color) ||
            !cells_color_ok(mode0_cells[i].ul_color))
            return false;
    cells_publish();
    return true;
}

void cells_set_height(uint8_t width, uint8_t height)
{
    term_set_height(&cells_term, width == 40 ? &cells_40.grid : &cells_80.grid, height);
    cells_stale = true;
}
