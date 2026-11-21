/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/screen.h"
#include "core/term/term.h"
#include "machine.h"
#include <assert.h>
#include <string.h>
#if TERM_CELLS
#include "core/vga/mode/mode.h"
#include "core/vga/pixel_format.h"
#endif

// The ring a screen's rows are laid out in (screen.h). Every machine keeps
// it for every screen, with cells or without.

void screen_lines_normal(screen_t *s, unsigned from, unsigned to)
{
    for (unsigned y = from; y < to; y++)
        s->line[screen_mem(s, (uint8_t)y)] = TERM_LINE_NORMAL;
}

void screen_all_normal(screen_t *s)
{
    memset(s->line, TERM_LINE_NORMAL, s->mem_rows);
}

void screen_init(screen_t *s, uint8_t rows, uint8_t *line, uint8_t *row_idx)
{
    s->rows = rows;
    s->mem_rows = (uint8_t)(row_idx ? rows + TERM_SPARE_ROWS : rows);
    s->line = line;
    s->row_idx = row_idx;
    s->cells = NULL;
    screen_all_normal(s);
    screen_ring_clear(s);
}

void screen_set_cells(screen_t *s, struct screen_cells *cells)
{
    s->cells = cells;
}

void screen_ring_clear(screen_t *s)
{
    s->y_offset = 0;
    s->hidden = 0;
    s->spare = s->rows;
    if (s->row_idx)
        for (unsigned y = 0; y < s->rows; y++)
            s->row_idx[y] = (uint8_t)y;
}

// Rows [lo, hi] of the screen swap their rows of memory end for end. With
// no ring, a slot's row of memory is the slot's own, so the rows' line
// attributes swap instead.
static void screen_reverse(screen_t *s, unsigned lo, unsigned hi)
{
    uint8_t *const ring = s->row_idx ? s->row_idx : s->line;
    for (; lo < hi; lo++, hi--)
    {
        uint8_t *a = &ring[screen_slot(s, (uint8_t)lo)];
        uint8_t *b = &ring[screen_slot(s, (uint8_t)hi)];
        const uint8_t m = *a;
        *a = *b;
        *b = m;
    }
}

// The rows turn k places toward the top: the first k reversed, the rest
// reversed, then all of them reversed. No row needs keeping aside, so a
// ring of any size turns in place.
void screen_rotate(screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up)
{
    const unsigned h = (unsigned)bot - top + 1;
    const unsigned k = up ? n : h - n;
    if (!k || k >= h)
        return;
    screen_reverse(s, top, top + k - 1);
    screen_reverse(s, top + k, bot);
    screen_reverse(s, top, bot);
}

// The ring turns a slot: up, row 0 is the row that was row 1; down, the row
// that was the last slot's.
static void screen_turn(screen_t *s, bool up)
{
    if (up)
        s->y_offset = (uint8_t)(s->y_offset + 1u == s->rows ? 0 : s->y_offset + 1u);
    else
        s->y_offset = (uint8_t)(s->y_offset ? s->y_offset - 1u : s->rows - 1u);
}

// The row of memory of row gone, which just went out, is the one a smooth
// scroll draws going. With keep, when row in, which comes in, is in the
// same slot, the spare row of memory takes that slot.
static uint8_t screen_keep(screen_t *s, uint8_t in, uint8_t gone, bool keep)
{
    const uint8_t slot = screen_slot(s, gone);
    const uint8_t ghost = s->row_idx ? s->row_idx[slot] : slot;
    if (keep && in == gone && s->spare != s->mem_rows)
    {
        s->row_idx[slot] = s->spare;
        s->spare = ghost;
    }
    return ghost;
}

uint8_t screen_ring_scroll(screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up, bool keep)
{
    screen_rotate(s, top, bot, n, up);
    const uint8_t edge = (uint8_t)(up ? bot - n + 1 : top + n - 1);
    const uint8_t ghost = screen_keep(s, edge, edge, keep);
    if (up)
        screen_lines_normal(s, edge, (unsigned)bot + 1);
    else
        screen_lines_normal(s, top, (unsigned)edge + 1);
    return ghost;
}

uint8_t screen_ring_index(screen_t *s, uint8_t height, bool up, bool keep)
{
    screen_turn(s, up);
    const uint8_t in = (uint8_t)(up ? height - 1 : 0);
    uint8_t ghost;
    if (up)
    {
        if (s->hidden < s->rows - height)
            s->hidden++;
        ghost = screen_keep(s, in, (uint8_t)(s->rows - 1), keep);
    }
    else
    {
        if (s->hidden)
            s->hidden--;
        ghost = screen_keep(s, in, (uint8_t)(height == s->rows ? 0 : height), keep);
    }
    screen_lines_normal(s, in, in + 1u);
    return ghost;
}

bool screen_ring_height(screen_t *s, uint8_t height, bool grow, bool top)
{
    if (!top)
    {
        if (grow)
            screen_lines_normal(s, height - 1u, height);
        return false;
    }
    screen_turn(s, !grow);
    if (!grow)
        s->hidden++;
    else if (s->hidden)
        s->hidden--;
    else
    {
        screen_lines_normal(s, 0, 1);
        return true;
    }
    return false;
}

void screen_ring_fit(screen_t *s, uint8_t height)
{
    const uint8_t room = (uint8_t)(s->rows - height);
    if (s->hidden > room)
        s->hidden = room;
}

#if TERM_CELLS

// The cell store: the cells of a screen that has them, which the grid
// (grid.c) changes through the screen_ functions, and what each row of them
// is still owed. draw.c makes the words mode 0 draws the size showing from,
// and keeps the colors, the blinks and the smooth scrolls.
//
// Designed to keep up with 115200 bps without flow control.
// The logic herein will make more sense if you remember this:

// 1. The screen data doesn't move when scrolling. Instead, the rows
//    are remapped through each screen's ring, y_offset and row_idx,
//    and mode 0's row words say where each row's cells are.
// 2. The screen doesn't fully clear immediately. To keep the UART
//    buffer from overflowing, lines are cleared in a background task
//    and checked as the cursor moves into them. Nothing fills more
//    than one row of cells in a single operation.
// 3. A cell's glyph and attributes are one word (mode0.h). The glyph
//    is where the font has it, so a scanline draws any cell with one
//    lookup. The attributes hold the lines through a cell (underline,
//    strike, overline), which a scanline reads only on the rows of the
//    glyph where a line can be, and there it branches once per cell
//    on the word being a bare glyph.
// 4. A cell's colors are in a table beside the cells. A cell names its
//    colors: a palette entry or a default color as an
//    entry of mode 0's palette, an RGB color as the pixel. The pen chooses
//    which as each cell is written, and a palette or default color that
//    changes afterwards is one write to the palette, which every cell
//    drawn with it shows on the next frame.

// Runs of cells. A cell is a word, and its colors are three halfwords in
// the table beside it (mode0.h), which go wherever the cell goes.
// Everything that writes or moves more than a cell does it here.

static inline uint32_t screen_cell_at(const mode0_cell_t *cell)
{
    return (uint32_t)(cell - mode0_cells);
}

// The colors of a cell.
static inline mode0_colors_t *screen_cell_colors(const mode0_cell_t *cell)
{
    return &mode0_colors[screen_cell_at(cell)];
}

// GCC turns a loop that copies or fills into a call of memmove or memset,
// and the C library's are slower here, a byte at a time.
#if defined(__GNUC__) && !defined(__clang__)
#define SCREEN_RUN __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define SCREEN_RUN
#endif

// The n cells at dst become `cell`, and their colors `colors`. The cells
// are written eight a pass. Two cells' colors are three words, so the
// colors are written four cells a pass from the first that is on a word.
static SCREEN_RUN void screen_run_fill(mode0_cell_t *dst, unsigned n, mode0_cell_t cell,
                                       mode0_colors_t colors)
{
    uint32_t w;
    memcpy(&w, &cell, sizeof(w));
    mode_word_t *d = (mode_word_t *)dst;
    unsigned i = n;
    for (; i >= 8; i -= 8, d += 8)
    {
        d[0] = w;
        d[1] = w;
        d[2] = w;
        d[3] = w;
        d[4] = w;
        d[5] = w;
        d[6] = w;
        d[7] = w;
    }
    for (; i; i--)
        *d++ = w;
    mode0_colors_t *c = screen_cell_colors(dst);
    i = n;
    if (i && (screen_cell_at(dst) & 1))
    {
        *c++ = colors;
        i--;
    }
    const mode0_colors_t two[2] = {colors, colors};
    uint32_t p[3];
    memcpy(p, two, sizeof(p));
    const uint32_t p0 = p[0], p1 = p[1], p2 = p[2];
    d = (mode_word_t *)c;
    for (; i >= 4; i -= 4, d += 6)
    {
        d[0] = p0;
        d[1] = p1;
        d[2] = p2;
        d[3] = p0;
        d[4] = p1;
        d[5] = p2;
    }
    for (; i >= 2; i -= 2, d += 3)
    {
        d[0] = p0;
        d[1] = p1;
        d[2] = p2;
    }
    if (i)
        *(mode0_colors_t *)d = colors;
}

// The n words at src move to dst. The two may overlap, so a move toward
// lower addresses goes up from the first word and one toward higher
// addresses goes down from the last, eight words a pass. The same move as
// ldmia and stmia of eight registers, and ldmdb and stmdb for the other
// way, measured 437 and 427 cycles for 160 words against this one's 514
// and 458. It is not used, so every machine runs one path, but it is there
// to take if the cycles are wanted.
static inline __attribute__((always_inline)) void
screen_words_move(mode_word_t *d, const mode_word_t *f, unsigned w)
{
    if (d < f)
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
    }
    else if (d > f)
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
    }
}

// The n cells at src, and their colors, move to dst. The colors are
// halfwords, three a cell, moved as words. Those of an odd count of cells
// are a halfword more than whole words: it goes first on the way down and
// last on the way up. A move by an odd count of cells has its words off a
// word boundary at one end, 699 cycles for a row of 80 moved a cell down
// and 610 up, against 244 for 40 cells moved by 40. Peeling a halfword so
// every read sits on a word measured no better there. A move by an even
// count from an odd cell, as from a column of one row to the same column
// of another, has both ends off a word together, and there a halfword
// peeled from each end puts every word on one.
static SCREEN_RUN void screen_run_move(mode0_cell_t *dst, const mode0_cell_t *src, unsigned n)
{
    screen_words_move((mode_word_t *)dst, (const mode_word_t *)src, n);
    uint16_t *cd = (uint16_t *)screen_cell_colors(dst);
    const uint16_t *cf = (const uint16_t *)screen_cell_colors(src);
    unsigned halves = 3 * n;
    if (halves && (screen_cell_at(dst) & screen_cell_at(src) & 1))
    {
        if (dst < src)
        {
            *cd++ = *cf++;
            halves--;
        }
        else
        {
            // The last halfword of the run, then the first, which is off
            // a word, after the words between them.
            halves--;
            if (halves & 1)
            {
                cd[halves] = cf[halves];
                halves--;
            }
            screen_words_move((mode_word_t *)(cd + 1), (const mode_word_t *)(cf + 1), halves / 2);
            cd[0] = cf[0];
            return;
        }
    }
    if ((halves & 1) && dst > src)
        cd[halves - 1] = cf[halves - 1];
    screen_words_move((mode_word_t *)cd, (const mode_word_t *)cf, halves / 2);
    if ((halves & 1) && dst < src)
        cd[halves - 1] = cf[halves - 1];
}

bool screen_moved;

/* draw.c's CELL chunk saves each screen's layout (screen_sst_put). The
 * cells and their colors are mode 0's (mode0.h). */
#define SCREEN_SST(X) \
    X(SKIP, screen_moved, "the canvas's load publishes everything")
SST_TABLE(screen_store, SCREEN_SST)

/* The cells' addresses are never written, because they are mode 0's, the
 * same in every build of a machine. */
void screen_sst_put(sst_cursor_t *c, screen_t *s)
{
    const uint8_t *const start = c->at;
    screen_cells_t *b = s->cells;
    screen_settle(s);
    for (int i = 0; i < s->rows; i++)
        sst_put_u8(c, s->row_idx[i]);
    sst_put_u8(c, s->spare);
    sst_put(c, s->line, s->mem_rows);
    /* What each row of memory is owed, and its record in 12 bytes. */
    for (int i = 0; i < s->mem_rows; i++)
    {
        sst_put_u8(c, b->owed[i]);
        if (b->owed[i] == SCREEN_OWED_SHIFT)
        {
            const screen_shift_t *h = &b->what[i].shift;
            sst_put_u8(c, h->from);
            sst_put_u8(c, h->to);
            sst_put_u8(c, h->end);
            sst_put_u8(c, h->cols);
            sst_put_u16(c, h->left_fg);
            sst_put_u16(c, h->left_bg);
            sst_put_u16(c, h->right_fg);
            sst_put_u16(c, h->right_bg);
        }
        else if (b->owed[i] == SCREEN_OWED_MARKS)
        {
            const screen_marks_t *k = &b->what[i].marks;
            sst_put_u8(c, k->from);
            sst_put_u8(c, k->to);
            sst_put_u8(c, k->set);
            sst_put_u8(c, k->clear);
            sst_put_u8(c, k->toggle);
            for (int n = 5; n < 12; n++)
                sst_put_u8(c, 0);
        }
        else if (b->owed[i] == SCREEN_OWED_FILL)
        {
            const screen_fill_t *e = &b->what[i].fill;
            sst_put_u16(c, e->cell.glyph);
            sst_put_u16(c, e->cell.attributes);
            sst_put_u16(c, e->colors.fg_color);
            sst_put_u16(c, e->colors.bg_color);
            sst_put_u16(c, e->colors.ul_color);
            sst_put_bool(c, e->selective);
            sst_put_u8(c, 0);
        }
        else
            for (int k = 0; k < 12; k++)
                sst_put_u8(c, 0);
    }
    sst_put_u8(c, s->y_offset);
    sst_put_u8(c, s->hidden);
    sst_put_bool(c, b->all_clean);
    assert(!sst_ok(c) || c->at - start == SCREEN_SST_SIZE);
    (void)start;
}

bool screen_sst_get(sst_cursor_t *c, screen_t *s)
{
    screen_cells_t *b = s->cells;
    bool ok = true;
    for (int i = 0; i < s->rows; i++)
        s->row_idx[i] = sst_get_u8(c);
    s->spare = sst_get_u8(c);
    sst_get(c, s->line, s->mem_rows);
    for (int i = 0; i < s->mem_rows; i++)
    {
        b->owed[i] = sst_get_u8(c);
        memset(&b->what[i], 0, sizeof(b->what[i]));
        if (b->owed[i] == SCREEN_OWED_SHIFT)
        {
            screen_shift_t *h = &b->what[i].shift;
            h->from = sst_get_u8(c);
            h->to = sst_get_u8(c);
            h->end = sst_get_u8(c);
            h->cols = sst_get_u8(c);
            h->left_fg = sst_get_u16(c);
            h->left_bg = sst_get_u16(c);
            h->right_fg = sst_get_u16(c);
            h->right_bg = sst_get_u16(c);
            /* The record indexes the row's cells. */
            ok &= h->cols <= b->width && h->end <= h->cols && h->to <= h->end &&
                  h->from + h->end - h->to <= h->cols &&
                  mode0_color_ok(h->left_fg) && mode0_color_ok(h->left_bg) &&
                  mode0_color_ok(h->right_fg) && mode0_color_ok(h->right_bg);
        }
        else if (b->owed[i] == SCREEN_OWED_MARKS)
        {
            screen_marks_t *k = &b->what[i].marks;
            k->from = sst_get_u8(c);
            k->to = sst_get_u8(c);
            k->set = sst_get_u8(c);
            k->clear = sst_get_u8(c);
            k->toggle = sst_get_u8(c);
            for (int n = 5; n < 12; n++)
                sst_get_u8(c);
            /* The record indexes the row's cells. */
            ok &= k->from <= k->to && k->to <= b->width;
        }
        else if (b->owed[i] == SCREEN_OWED_FILL)
        {
            screen_fill_t *e = &b->what[i].fill;
            e->cell.glyph = sst_get_u16(c);
            e->cell.attributes = sst_get_u16(c);
            e->colors.fg_color = sst_get_u16(c);
            e->colors.bg_color = sst_get_u16(c);
            e->colors.ul_color = sst_get_u16(c);
            e->selective = sst_get_bool(c);
            sst_get_u8(c);
            ok &= mode0_glyph_ok(e->cell.glyph, b->width == 40 ? 8 : 16) &&
                  mode0_color_ok(e->colors.fg_color) &&
                  mode0_color_ok(e->colors.bg_color) && mode0_color_ok(e->colors.ul_color);
        }
        else
        {
            ok &= b->owed[i] == SCREEN_OWED_NONE;
            for (int k = 0; k < 12; k++)
                sst_get_u8(c);
        }
    }
    s->y_offset = sst_get_u8(c);
    s->hidden = sst_get_u8(c);
    b->all_clean = sst_get_bool(c);
    b->later.n = 0;
    /* Everything here indexes the cell array. row_idx is the ring and
     * y_offset is added to it before the lookup, so both are bounded, and
     * the slots and the spare name every row of memory once. With no spare
     * row, the spare is none. */
    if (!sst_ok(c))
        return false;
    if (!ok || s->y_offset >= s->rows || s->hidden >= s->rows)
        return sst_refuse(c, "a screen's layout out of range");
    bool seen[TERM_MEM_ROWS] = {false};
    for (int i = 0; i < s->mem_rows; i++)
    {
        uint8_t m = i < s->rows ? s->row_idx[i] : s->spare;
        if (m >= s->mem_rows || seen[m])
            return sst_refuse(c, "a screen's rows out of order");
        seen[m] = true;
    }
    if (s->mem_rows == s->rows && s->spare != s->mem_rows)
        return sst_refuse(c, "a screen's rows out of order");
    return true;
}

// The screen with the scroll it is owed done, for everything that changes
// a cell. False for a screen with no cells.
static inline bool screen_settled(screen_t *s)
{
    if (!s->cells)
        return false;
    if (s->cells->later.n)
        screen_settle(s);
    return true;
}

// A scroll, an index, a clear or a change of height of a screen with cells
// ends the smooth scroll of its grid.
static inline void screen_cancel(screen_t *s)
{
    s->cells->cancels++;
}

/* ---- colors ------------------------------------------------------------ */

// A cell's color at half brightness: an RGB pixel halved, or the faint twin
// of an entry.
static uint16_t screen_color_faint(uint16_t color)
{
    if (color & SCANVIDEO_ALPHA_MASK)
        return mode0_faint(color);
    return (uint16_t)(color + MODE0_ENTRY(MODE0_ENTRIES, false));
}

// A pen color before bold, faint and iCE colors, as a cell holds it: the
// palette entry, the RGB pixel, or the default (dflt, MODE0_DEFAULT_FG or
// _BG).
static uint16_t screen_base(const term_color_t *c, unsigned dflt)
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
static uint16_t screen_pen_fg(const term_pen_t *pen)
{
    uint16_t fg;
    if (pen->bold && pen->fg.kind == TERM_COLOR_INDEX && pen->fg.index < 8)
        fg = MODE0_ENTRY(pen->fg.index + 8, false);
    else if (pen->bold && pen->fg.kind == TERM_COLOR_DEFAULT)
        fg = MODE0_ENTRY(15, false);
    else
        fg = screen_base(&pen->fg, MODE0_DEFAULT_FG);
    return pen->faint ? screen_color_faint(fg) : fg;
}

static uint16_t screen_pen_bg(const term_t *term, const term_pen_t *pen)
{
    bool bright = term->ice_colors && (pen->sgr_attr & TERM_ATTR_ANY_BLINK);
    if (bright && pen->bg.kind == TERM_COLOR_INDEX && pen->bg.index < 8)
        return MODE0_ENTRY(pen->bg.index + 8, false);
    if (bright && pen->bg.kind == TERM_COLOR_DEFAULT)
        return MODE0_ENTRY(8, false);
    return screen_base(&pen->bg, MODE0_DEFAULT_BG);
}

/* ---- cells and rows ---------------------------------------------------- */

// Fill [start, end) of `row`: every cell becomes the fill's cell, in its
// colors. A selective erase leaves the guarded cells as they are.
static void fill_glyph(mode0_cell_t *row, unsigned start, unsigned end,
                       const screen_fill_t *e)
{
    if (!e->selective)
    {
        screen_run_fill(row + start, end - start, e->cell, e->colors);
        return;
    }
    mode0_colors_t *colors = screen_cell_colors(row);
    for (unsigned i = start; i < end; i++)
        if (!(row[i].attributes & MODE0_ATTR_GUARD))
        {
            row[i] = e->cell;
            colors[i] = e->colors;
        }
}

// A blank cell in these colors: a space, or DECALN's E, with no attributes
// and no guard. The foreground matters too, because the cursor is drawn
// from the cell it sits on.
static screen_fill_t screen_blank(uint8_t glyph, uint16_t fg, uint16_t bg, bool selective)
{
    const screen_fill_t e = {{glyph, 0}, {fg, bg, fg}, selective};
    return e;
}

// Two fills that leave a row the same.
static inline bool screen_fill_same(const screen_fill_t *a, const screen_fill_t *b)
{
    return a->selective == b->selective && a->cell.glyph == b->cell.glyph &&
           a->cell.attributes == b->cell.attributes &&
           a->colors.fg_color == b->colors.fg_color &&
           a->colors.bg_color == b->colors.bg_color &&
           a->colors.ul_color == b->colors.ul_color;
}

// An erase in the pen's colors, selective or not. An erase takes the pen's
// colors, not reversed, and none of its attributes, as xterm and Linux do.
static screen_fill_t screen_pen_erase(const term_t *term, bool selective)
{
    const term_pen_t *pen = term_pen(term);
    return screen_blank(' ', screen_pen_fg(pen), screen_pen_bg(term, pen), selective);
}

// Erase [start, end) of `row` in the pen's colors.
static void screen_fill_erased(term_t *term, mode0_cell_t *row, unsigned start, unsigned end,
                               bool selective)
{
    const screen_fill_t e = screen_pen_erase(term, selective);
    fill_glyph(row, start, end, &e);
}

// Bold on or off for a cell's ink, which is a palette entry it can be told
// of: on, entries 0 to 7 and the default foreground become their bright
// entries, as the pen draws them; off, an ink that bold brightened goes
// back. An RGB ink, or one that was bright without bold, stays as it is.
// Returns the cell's attributes.
static uint16_t screen_ink_bold(uint16_t *ink, uint16_t attributes, bool on)
{
    if (*ink & SCANVIDEO_ALPHA_MASK)
        return attributes;
    unsigned entry = *ink >> MODE0_ENTRY_SHIFT;
    const bool faint = entry >= MODE0_ENTRIES;
    if (faint)
        entry -= MODE0_ENTRIES;
    if (on && !(attributes & MODE0_ATTR_BOLD))
    {
        if (entry < 8)
        {
            entry += 8;
            attributes |= MODE0_ATTR_BOLD;
        }
        else if (entry == MODE0_DEFAULT_FG)
        {
            entry = 15;
            attributes |= MODE0_ATTR_BOLD | MODE0_ATTR_BOLD_DEFAULT;
        }
    }
    else if (!on && (attributes & MODE0_ATTR_BOLD))
    {
        if (attributes & MODE0_ATTR_BOLD_DEFAULT)
            entry = MODE0_DEFAULT_FG;
        else if (entry >= 8 && entry < 16)
            entry -= 8;
        attributes &= (uint16_t)~(MODE0_ATTR_BOLD | MODE0_ATTR_BOLD_DEFAULT);
    }
    *ink = MODE0_ENTRY(entry, faint);
    return attributes;
}

// Change the attributes of columns [from, to) of a row: set, clear, then
// reverse. Each cell's are worked out from what it has, so a cell that
// already has what is asked is left alone. A cell whose two colors are the
// same, a concealed one for one, is not made bold, which would show it. An
// underline set here is a single one in the foreground's color.
static void screen_row_marks(mode0_cell_t *row, unsigned from, unsigned to,
                             unsigned set, unsigned clear, unsigned toggle)
{
    mode0_colors_t *colors = screen_cell_colors(row);
    for (unsigned i = from; i < to; i++)
    {
        uint16_t a = row[i].attributes;
        mode0_colors_t *c = &colors[i];
        const unsigned has = ((a & MODE0_ATTR_BOLD) ? SCREEN_MARK_BOLD : 0) |
                             ((a & TERM_ATTR_UL_MASK) ? SCREEN_MARK_UNDERLINE : 0) |
                             ((a & TERM_ATTR_ANY_BLINK) ? SCREEN_MARK_BLINK : 0) |
                             ((a & MODE0_ATTR_REVERSE) ? SCREEN_MARK_REVERSE : 0);
        const unsigned want = ((has | set) & ~clear) ^ toggle;
        const unsigned change = has ^ want;
        if (!change)
            continue;
        if (change & SCREEN_MARK_REVERSE)
        {
            const uint16_t fg = c->fg_color;
            c->fg_color = c->bg_color;
            c->bg_color = fg;
            a ^= MODE0_ATTR_REVERSE;
        }
        if ((change & SCREEN_MARK_BOLD) && c->fg_color != c->bg_color)
            a = screen_ink_bold((a & MODE0_ATTR_REVERSE) ? &c->bg_color : &c->fg_color, a,
                                want & SCREEN_MARK_BOLD);
        if (change & SCREEN_MARK_UNDERLINE)
        {
            a &= (uint16_t)~(TERM_ATTR_UL_MASK | MODE0_ATTR_UL_COLOR);
            if (want & SCREEN_MARK_UNDERLINE)
                a |= TERM_ATTR_UL(TERM_UL_SINGLE);
        }
        if (change & SCREEN_MARK_BLINK)
        {
            a &= (uint16_t)~TERM_ATTR_ANY_BLINK;
            if (want & SCREEN_MARK_BLINK)
                a |= TERM_ATTR_BLINK;
        }
        if (!(a & MODE0_ATTR_UL_COLOR))
            c->ul_color = c->fg_color;
        row[i].attributes = a;
    }
}

// Pay row m of memory what it is owed. A shift moves the survivors, then
// blanks the columns either side of where they landed.
static void screen_pay(screen_cells_t *buf, uint8_t m)
{
    mode0_cell_t *row = buf->mem + (uint32_t)m * buf->width;
    const uint8_t owed = buf->owed[m];
    buf->owed[m] = SCREEN_OWED_NONE;
    if (owed == SCREEN_OWED_FILL)
        fill_glyph(row, 0, buf->width, &buf->what[m].fill);
    else if (owed == SCREEN_OWED_MARKS)
    {
        const screen_marks_t *k = &buf->what[m].marks;
        screen_row_marks(row, k->from, k->to, k->set, k->clear, k->toggle);
    }
    else if (owed == SCREEN_OWED_SHIFT)
    {
        const screen_shift_t *s = &buf->what[m].shift;
        screen_run_move(row + s->to, row + s->from, (unsigned)(s->end - s->to));
        if (s->to)
        {
            const screen_fill_t e = screen_blank(' ', s->left_fg, s->left_bg, false);
            screen_run_fill(row, s->to, e.cell, e.colors);
        }
        if (s->end < s->cols)
        {
            const screen_fill_t e = screen_blank(' ', s->right_fg, s->right_bg, false);
            screen_run_fill(row + s->end, (unsigned)(s->cols - s->end), e.cell, e.colors);
        }
    }
}

// Make sure you call this any time you change rows.
// It pays the row what it is still owed.
static void screen_clean_line(screen_t *s, uint8_t y)
{
    const uint8_t m = screen_mem(s, y);
    if (s->cells->owed[m])
        screen_pay(s->cells, m);
}

// Clean one dirty row of a screen. Returns true if a row was cleaned this
// call, false if the screen was already fully clean. Operates on an
// explicit screen so the draw task can drain the inactive (alt) screen
// too -- otherwise the lazy mark on RIS would leave alt dirty until ?47
// swaps it in.
bool screen_tidy(screen_t *s, uint8_t height)
{
    screen_cells_t *buf = s->cells;
    if (buf->all_clean)
        return false;
    for (uint8_t i = 0; i < height; i++)
    {
        const uint8_t m = screen_mem(s, i);
        if (buf->owed[m])
        {
            screen_pay(buf, m);
            return true;
        }
    }
    buf->all_clean = true;
    return false;
}

// Rows [from, to) of the screen are owed this fill, in place of whatever
// they were owed. A row owed a whole fill has every cell guarded or none,
// so a selective erase of it spares all of that fill or lands whole. A row
// owed a shift or a change of attributes is paid before a selective erase,
// which goes by its guards.
static void screen_mark_rows_with(screen_t *s, uint8_t from, uint8_t to,
                                  const screen_fill_t *e)
{
    screen_cells_t *b = s->cells;
    for (uint8_t i = from; i < to; i++)
    {
        const uint8_t m = screen_mem(s, i);
        screen_fill_t *fill = &b->what[m].fill;
        if (e->selective && b->owed[m] >= SCREEN_OWED_SHIFT)
            screen_pay(b, m);
        else if (e->selective && b->owed[m] == SCREEN_OWED_FILL && !fill->selective)
        {
            if (fill->cell.attributes & MODE0_ATTR_GUARD)
                continue;
            *fill = *e;
            fill->selective = false;
            continue;
        }
        b->owed[m] = SCREEN_OWED_FILL;
        *fill = *e;
    }
    b->all_clean = false;
}

// Lazy-erase rows [from, to) to spaces in the default colors.
static void screen_mark_rows_default(screen_t *s, uint8_t from, uint8_t to)
{
    const screen_fill_t e = screen_blank(' ', MODE0_ENTRY(MODE0_DEFAULT_FG, false),
                                         MODE0_ENTRY(MODE0_DEFAULT_BG, false), false);
    screen_mark_rows_with(s, from, to, &e);
}

// Lazy-erase rows [from, to) of a screen in the pen's colors.
static void screen_mark_rows_erase(term_t *term, screen_t *s, uint8_t from, uint8_t to,
                                   bool selective)
{
    const screen_fill_t e = screen_pen_erase(term, selective);
    screen_mark_rows_with(s, from, to, &e);
}

// Lazy clear of the given screen: its ring in order, and every visible row
// marked dirty with the given erase. A scroll the screen was owed is
// forgotten with the cells it would have moved. No cell memory is touched
// here -- draw.c's task drains dirty rows in the background (one row per
// tick, on both screens), and the active screen's cursor row is also
// cleaned synchronously when the terminal touches it.
static void screen_mark_erased(screen_t *s, uint8_t height, const screen_fill_t *e)
{
    screen_cells_t *b = s->cells;
    screen_ring_clear(s);
    b->later.n = 0;
    for (uint8_t m = 0; m < s->mem_rows; m++)
        b->owed[m] = SCREEN_OWED_NONE;
    screen_mark_rows_with(s, 0, height, e);
}

/* ---- what the grid asks of the cells ----------------------------------- */

void screen_touch(screen_t *s, uint8_t y)
{
    if (s->cells)
        screen_clean_line(s, y);
}

// The cell a glyph lands as on a screen: the pen's colors and attributes,
// through the active character set. The cell holds where the font has the
// glyph (mode0.h): DEC Special Graphics for that set's 0x5F to 0x7E, DEC
// Technical for that set's 0x21 to 0x7E, the italics for SGR 3's ASCII, the
// code page's otherwise. The UK set's pound
// sign is the DEC set's 0x7D. SGR 7 and 8 swap and hide at the cell, and in
// iCE colors mode (?33) the blink bits are left off the cell: the bright
// background is the blink. An underline SGR 58 colored is flagged, so the
// renderer reads its color; any other holds the foreground's color. A cell
// written while DECSCA or SPA protects is guarded. The cell says when bold
// brightened its ink and when its colors are swapped, so DECCARA and
// DECRARA can undo either; a concealed cell has no ink to say it of. The
// glyph's rows are the font's for the screen's width: 8 on the 40-column
// screen, drawn on the 320-wide canvases, and 16 on the 80.
static screen_fill_t screen_glyph_cell(term_t *term, const screen_t *s, uint8_t byte)
{
    const term_pen_t *pen = term_pen(term);
    const unsigned rows = s->cells->width == 40 ? 8 : 16;
    screen_fill_t e;
    uint16_t f = screen_pen_fg(pen);
    uint16_t b = screen_pen_bg(term, pen);
    if (pen->reverse)
    {
        uint16_t tmp = f;
        f = b;
        b = tmp;
    }
    if (pen->conceal)
        f = b;
    e.colors.fg_color = f;
    e.colors.bg_color = b;
    uint16_t cell_mask = TERM_ATTR_CELL_MASK;
    if (term->ice_colors)
        cell_mask &= (uint16_t)~TERM_ATTR_ANY_BLINK;
    uint16_t attributes = (uint16_t)(pen->sgr_attr & cell_mask);
    if (pen->ul_set && (attributes & TERM_ATTR_UL_MASK))
        attributes |= MODE0_ATTR_UL_COLOR;
    if (term_protecting(term))
        attributes |= MODE0_ATTR_GUARD;
    if (pen->reverse)
        attributes |= MODE0_ATTR_REVERSE;
    if (pen->bold && !pen->conceal)
    {
        if (pen->fg.kind == TERM_COLOR_INDEX && pen->fg.index < 8)
            attributes |= MODE0_ATTR_BOLD;
        else if (pen->fg.kind == TERM_COLOR_DEFAULT)
            attributes |= MODE0_ATTR_BOLD | MODE0_ATTR_BOLD_DEFAULT;
    }
    uint16_t glyph = byte;
    uint8_t set = pen->g[pen->ss ? pen->ss : pen->gl];
    if (set == TERM_CS_DEC && byte >= 0x5F && byte <= 0x7E)
        glyph = MODE0_GLYPH_DEC(rows, byte);
    else if (set == TERM_CS_TECH && byte >= 0x21 && byte <= 0x7E)
        glyph = MODE0_GLYPH_TECH(rows, byte);
    else if (set == TERM_CS_UK && byte == '#')
        glyph = MODE0_GLYPH_DEC(rows, 0x7D);
    else if ((pen->sgr_attr & TERM_ATTR_ITALIC) && byte < 0x80)
        glyph = MODE0_GLYPH_ITALIC(rows, byte);
    e.cell.glyph = glyph;
    e.cell.attributes = attributes;
    e.colors.ul_color = pen->ul_set ? screen_base(&pen->ul, MODE0_DEFAULT_FG) : f;
    e.selective = false;
    return e;
}

void screen_glyphs(term_t *term, screen_t *s, uint8_t y, uint8_t x, uint8_t byte, unsigned n)
{
    if (!screen_settled(s))
        return;
    const screen_fill_t e = screen_glyph_cell(term, s, byte);
    mode0_cell_t *at = screen_row(s, y) + x;
    if (n == 1)
    {
        // A character: its word and its three colors.
        *at = e.cell;
        *screen_cell_colors(at) = e.colors;
        return;
    }
    screen_run_fill(at, n, e.cell, e.colors);
}

void screen_wipe(term_t *term, screen_t *s, uint8_t y, unsigned from, unsigned to,
                 bool selective)
{
    (void)term;
    if (!screen_settled(s))
        return;
    mode0_cell_t *row = screen_row(s, y);
    for (unsigned x = from; x < to; x++)
        if (!selective || !(row[x].attributes & MODE0_ATTR_GUARD))
            row[x].glyph = ' ';
}

void screen_fill_row(term_t *term, screen_t *s, uint8_t y, uint8_t byte)
{
    if (!screen_settled(s))
        return;
    const screen_fill_t e = screen_glyph_cell(term, s, byte);
    screen_mark_rows_with(s, y, (uint8_t)(y + 1), &e);
}

void screen_erase(term_t *term, screen_t *s, uint8_t y, unsigned from, unsigned to,
                  bool selective)
{
    if (!screen_settled(s))
        return;
    screen_fill_erased(term, screen_row(s, y), from, to, selective);
}

// Columns [x, end) of a row move n right, erased cells coming in at x; or n
// left, erased cells coming in before end.
static void screen_row_insert(term_t *term, mode0_cell_t *row, unsigned x, unsigned end,
                              unsigned n)
{
    screen_run_move(&row[x + n], &row[x], end - x - n);
    screen_fill_erased(term, row, x, x + n, false);
}

static void screen_row_delete(term_t *term, mode0_cell_t *row, unsigned x, unsigned end,
                              unsigned n)
{
    screen_run_move(&row[x], &row[x + n], end - x - n);
    screen_fill_erased(term, row, end - n, end, false);
}

void screen_insert(term_t *term, screen_t *s, uint8_t y, uint8_t x, unsigned end, unsigned n)
{
    if (!screen_settled(s))
        return;
    screen_row_insert(term, screen_row(s, y), x, end, n);
}

void screen_delete(term_t *term, screen_t *s, uint8_t y, uint8_t x, unsigned end, unsigned n)
{
    if (!screen_settled(s))
        return;
    screen_row_delete(term, screen_row(s, y), x, end, n);
}

void screen_copy_cells(term_t *term, screen_t *s, uint8_t sy, unsigned sx, uint8_t dy,
                       unsigned dx, unsigned n)
{
    (void)term;
    if (!screen_settled(s))
        return;
    screen_clean_line(s, sy);
    screen_clean_line(s, dy);
    screen_run_move(screen_row(s, dy) + dx, screen_row(s, sy) + sx, n);
}

// The row is only owed the change, after whatever it was owed before: no
// cell changes until the row is paid. The cursor's row is never left owing.
// In iCE colors mode (?33) no cell blinks, so none is made to.
void screen_attrs(term_t *term, screen_t *s, uint8_t y, unsigned from, unsigned to,
                  unsigned set, unsigned clear, unsigned toggle)
{
    if (!screen_settled(s))
        return;
    screen_cells_t *buf = s->cells;
    const uint8_t m = screen_mem(s, y);
    if (term->ice_colors)
    {
        set &= ~SCREEN_MARK_BLINK;
        toggle &= ~SCREEN_MARK_BLINK;
    }
    if (buf->owed[m])
        screen_pay(buf, m);
    const screen_marks_t marks = {(uint8_t)from, (uint8_t)to, (uint8_t)set, (uint8_t)clear,
                                  (uint8_t)toggle};
    buf->what[m].marks = marks;
    buf->owed[m] = SCREEN_OWED_MARKS;
    buf->all_clean = false;
    if (y == s->pos.y)
        screen_pay(buf, m);
}

// One more shift of n columns, with blanks in these colors, added to the
// shift a row of cols columns is owed.
static inline void screen_shift_add(screen_shift_t *s, uint8_t cols, uint8_t n, bool left,
                                    uint16_t fg, uint16_t bg)
{
    if (left)
    {
        if (s->to >= n)
            s->to = (uint8_t)(s->to - n);
        else
        {
            s->from = (uint8_t)(s->from + n - s->to);
            s->to = 0;
        }
        s->end = s->end > n ? (uint8_t)(s->end - n) : 0;
        s->right_fg = fg;
        s->right_bg = bg;
    }
    else
    {
        s->to = (uint8_t)(s->to + n < cols ? s->to + n : cols);
        s->end = (uint8_t)(s->end + n < cols ? s->end + n : cols);
        s->left_fg = fg;
        s->left_bg = bg;
    }
    if (s->to == s->end)
        s->from = 0;
}

// SL and SR only mark the rows: each is owed the shift, and no cell moves
// until the row is paid. A row that already owes a shift has this one
// added to its record. It is paid first only when this shift's blanks
// would sit beside blanks of another color on the same side. A row owed a
// whole fill of the very cells the shift brings in is left to it: shifted,
// it is the same row. Any other fill, and a change of attributes, is paid
// first. The cursor's row is
// never left owing. A row of the full width that owes nothing, which is
// most rows, takes a record made once for them all. Between left and right
// margins a row is owed nothing new: the cells between them move now.
void screen_shift(term_t *term, screen_t *sc, uint8_t top, uint8_t bot, unsigned from,
                  unsigned end, unsigned count, bool left)
{
    if (!screen_settled(sc))
        return;
    screen_cells_t *buf = sc->cells;
    if (from || end != buf->width)
    {
        for (uint8_t y = top; y <= bot; y++)
        {
            const unsigned cols = screen_size(sc, y) ? buf->width / 2u : buf->width;
            const unsigned e = end < cols ? end : cols;
            if (from >= e)
                continue;
            const unsigned n = count < e - from ? count : e - from;
            screen_clean_line(sc, y);
            if (left)
                screen_row_delete(term, screen_row(sc, y), from, e, n);
            else
                screen_row_insert(term, screen_row(sc, y), from, e, n);
        }
        return;
    }
    const screen_fill_t in = screen_pen_erase(term, false);
    const uint16_t fg = in.colors.fg_color, bg = in.colors.bg_color;
    const uint8_t width = buf->width;
    screen_shift_t whole = {0, 0, width, width, fg, bg, fg, bg};
    screen_shift_add(&whole, width, count < width ? (uint8_t)count : width, left, fg, bg);
    for (uint8_t y = top; y <= bot; y++)
    {
        const uint8_t m = screen_mem(sc, y);
        screen_shift_t *s = &buf->what[m].shift;
        const uint8_t owed = buf->owed[m];
        const uint8_t size = screen_size(sc, y);
        if (owed == SCREEN_OWED_NONE && !size)
        {
            buf->owed[m] = SCREEN_OWED_SHIFT;
            *s = whole;
            continue;
        }
        const uint8_t cols = size ? width / 2u : width;
        const uint8_t n = count < cols ? (uint8_t)count : cols;
        screen_shift_t r = {0, 0, cols, cols, fg, bg, fg, bg};
        if (owed == SCREEN_OWED_FILL)
        {
            const screen_fill_t *fill = &buf->what[m].fill;
            if (screen_fill_same(fill, &in))
                continue;
            screen_pay(buf, m);
        }
        else if (owed == SCREEN_OWED_SHIFT)
        {
            if (n < cols && (left ? s->end < cols && (s->right_fg != fg || s->right_bg != bg)
                                  : s->to && (s->left_fg != fg || s->left_bg != bg)))
                screen_pay(buf, m);
            else
                r = *s;
        }
        else if (owed == SCREEN_OWED_MARKS)
            screen_pay(buf, m);
        screen_shift_add(&r, cols, n, left, fg, bg);
        buf->owed[m] = SCREEN_OWED_SHIFT;
        *s = r;
    }
    buf->all_clean = false;
    screen_clean_line(sc, sc->pos.y);
}

void screen_erase_rows(term_t *term, screen_t *s, uint8_t from, uint8_t to, bool selective)
{
    if (!screen_settled(s))
        return;
    screen_mark_rows_erase(term, s, from, to, selective);
}

void screen_align(term_t *term, screen_t *s, uint8_t height)
{
    (void)term;
    if (!screen_settled(s))
        return;
    const screen_fill_t e = screen_blank('E', MODE0_ENTRY(MODE0_DEFAULT_FG, false),
                                         MODE0_ENTRY(MODE0_DEFAULT_BG, false), false);
    screen_mark_rows_with(s, 0, height, &e);
}

// The rows move round the ring, which takes what each row of memory is
// owed with it, and the n rows coming in are lazy-erased in the pen's
// colors, after the ring has kept the row going out. A screen with no cells
// has only its ring.
uint8_t screen_scroll(term_t *term, screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up,
                      bool keep)
{
    if (!screen_settled(s))
        return screen_ring_scroll(s, top, bot, n, up, keep);
    screen_cancel(s);
    const uint8_t ghost = screen_ring_scroll(s, top, bot, n, up, keep);
    if (up)
        screen_mark_rows_erase(term, s, (uint8_t)(bot - n + 1), (uint8_t)(bot + 1), false);
    else
        screen_mark_rows_erase(term, s, top, (uint8_t)(top + n), false);
    return ghost;
}

// The cells of a row either side of columns [from, end), kept while the
// rows move round. No more than this many are ever kept: with more either
// side than between, the cells between are the ones copied.
#define SCREEN_STRIP_MAX 40
typedef struct
{
    mode0_cell_t cell[SCREEN_STRIP_MAX];
    mode0_colors_t colors[SCREEN_STRIP_MAX];
} screen_strip_t;

static SCREEN_RUN void screen_strip_keep(screen_strip_t *strip, const mode0_cell_t *row,
                                         unsigned from, unsigned end, unsigned width)
{
    const mode0_colors_t *colors = screen_cell_colors(row);
    unsigned k = 0;
    for (unsigned i = 0; i < from; i++, k++)
    {
        strip->cell[k] = row[i];
        strip->colors[k] = colors[i];
    }
    for (unsigned i = end; i < width; i++, k++)
    {
        strip->cell[k] = row[i];
        strip->colors[k] = colors[i];
    }
}

static SCREEN_RUN void screen_strip_put(const screen_strip_t *strip, mode0_cell_t *row,
                                        unsigned from, unsigned end, unsigned width)
{
    mode0_colors_t *colors = screen_cell_colors(row);
    unsigned k = 0;
    for (unsigned i = 0; i < from; i++, k++)
    {
        row[i] = strip->cell[k];
        colors[i] = strip->colors[k];
    }
    for (unsigned i = end; i < width; i++, k++)
    {
        row[i] = strip->cell[k];
        colors[i] = strip->colors[k];
    }
}

static SCREEN_RUN void screen_strip_move(mode0_cell_t *to, const mode0_cell_t *row,
                                         unsigned from, unsigned end, unsigned width)
{
    mode0_colors_t *tc = screen_cell_colors(to);
    const mode0_colors_t *rc = screen_cell_colors(row);
    if (from > 4)
        screen_run_move(to, row, from);
    else
        for (unsigned i = 0; i < from; i++)
        {
            to[i] = row[i];
            tc[i] = rc[i];
        }
    if (width - end > 4)
        screen_run_move(to + end, row + end, width - end);
    else
        for (unsigned i = end; i < width; i++)
        {
            to[i] = row[i];
            tc[i] = rc[i];
        }
}

// Do the scroll a screen is owed. A row cannot move, as it does in a scroll
// of whole rows, without taking the cells either side of the columns with
// it. So the cells between the columns are copied up or down a row at a
// time; or, when there are fewer cells either side, the rows move and the
// cells either side, and the line attributes, go back to the rows they were
// on, each row's to the next round the region. No row of the region is owed
// anything: they were
// paid when the scroll came to be owed, and everything that changes a cell
// does the scroll first.
void screen_settle(screen_t *s)
{
    screen_cells_t *buf = s->cells;
    const screen_later_t l = buf->later;
    if (!l.n)
        return;
    buf->later.n = 0;
    screen_moved = true;
    const unsigned top = l.top, bot = l.bot, from = l.from, end = l.end, n = l.n;
    const unsigned h = bot - top + 1;
    const unsigned width = buf->width, inside = end - from, outside = width - inside;
    if (n < h && (outside > SCREEN_STRIP_MAX || inside * (h - n) <= outside * h))
    {
        for (unsigned i = 0; i < h - n; i++)
        {
            const unsigned to = l.up ? top + i : bot - i;
            screen_run_move(screen_row(s, (uint8_t)to) + from,
                            screen_row(s, (uint8_t)(l.up ? to + n : to - n)) + from, inside);
        }
    }
    else if (n < h)
    {
        screen_rotate(s, (uint8_t)top, (uint8_t)bot, (uint8_t)n, l.up);
        // Row j of the region now has the cells either side, and the line
        // attributes, that belong on row j + step, counted round the region.
        const unsigned step = l.up ? n : h - n;
        screen_strip_t strip;
        for (unsigned first = 0, moved = 0; moved < h; first++)
        {
            mode0_cell_t *row = screen_row(s, (uint8_t)(top + first));
            uint8_t *line = &s->line[screen_mem(s, (uint8_t)(top + first))];
            const uint8_t kept = *line;
            screen_strip_keep(&strip, row, from, end, width);
            for (unsigned j = first;;)
            {
                unsigned src = j >= step ? j - step : j + h - step;
                moved++;
                if (src == first)
                {
                    screen_strip_put(&strip, row, from, end, width);
                    *line = kept;
                    break;
                }
                const uint8_t m = screen_mem(s, (uint8_t)(top + src));
                mode0_cell_t *other = screen_row(s, (uint8_t)(top + src));
                screen_strip_move(row, other, from, end, width);
                *line = s->line[m];
                line = &s->line[m];
                row = other;
                j = src;
            }
        }
    }
    for (unsigned i = 0; i < n; i++)
        fill_glyph(screen_row(s, (uint8_t)(l.up ? bot - i : top + i)), from, end, &l.in);
}

// The screen is only owed the scroll. One of the same columns the same way
// that it already owes is added to; any other is done first. Every row of
// the region is paid what it is owed, which is owed to a whole row.
void screen_scroll_cols(term_t *term, screen_t *s, uint8_t top, uint8_t bot, unsigned from,
                        unsigned end, uint8_t n, bool up)
{
    if (!s->cells)
        return;
    screen_later_t *l = &s->cells->later;
    const screen_fill_t in = screen_pen_erase(term, false);
    const unsigned h = (unsigned)bot - top + 1;
    if (l->n && l->top == top && l->bot == bot && l->from == from && l->end == end &&
        l->up == up && screen_fill_same(&l->in, &in))
    {
        l->n = (uint8_t)(l->n + n < h ? l->n + n : h);
        return;
    }
    screen_settle(s);
    for (unsigned y = top; y <= bot; y++)
        screen_clean_line(s, (uint8_t)y);
    const screen_later_t later = {top, bot, (uint8_t)from, (uint8_t)end, n, up, in};
    *l = later;
}

// The fast scroll: the viewport rotates through y_offset, and the row
// coming in is lazy-erased in the pen's colors, after the ring has kept the
// row going out.
uint8_t screen_index(term_t *term, screen_t *s, uint8_t height, bool up, bool keep)
{
    if (!screen_settled(s))
        return screen_ring_index(s, height, up, keep);
    screen_cancel(s);
    const uint8_t ghost = screen_ring_index(s, height, up, keep);
    if (up)
        screen_mark_rows_erase(term, s, (uint8_t)(height - 1), height, false);
    else
        screen_mark_rows_erase(term, s, 0, 1, false);
    return ghost;
}

void screen_clear(term_t *term, screen_t *s, uint8_t height)
{
    if (!s->cells)
    {
        screen_ring_clear(s);
        return;
    }
    const screen_fill_t e = screen_pen_erase(term, false);
    screen_cancel(s);
    screen_mark_erased(s, height, &e);
}

void screen_height(screen_t *s, uint8_t height, bool grow, bool top)
{
    if (!screen_settled(s))
    {
        screen_ring_height(s, height, grow, top);
        return;
    }
    screen_cancel(s);
    // Other rows are in view now, and one of them may be owed a fill from
    // before it left.
    s->cells->all_clean = false;
    // With top, a grow moves everything down a row and the row above comes
    // into view: a row of scrollback, which comes back as it left with the
    // fill it was still owed, or else a blank one. A shrink sends the top
    // row out of view to join the scrollback.
    if (screen_ring_height(s, height, grow, top))
        screen_mark_rows_default(s, 0, 1);
    if (top)
        // The cursor's row is another row now, and is never owed a fill.
        screen_clean_line(s, s->pos.y);
    else if (grow)
        // Expose one fresh row at the bottom. The cleaner task drains it in
        // the background; the worst case is one row of stale content for a
        // frame or two.
        screen_mark_rows_default(s, (uint8_t)(height - 1), height);
    // Otherwise the bottom row is no longer visible; the cleaner only drains
    // visible rows. Leave it -- a later grow that re-exposes the row will
    // mark it dirty.
}

// The rows above the view and the rows below it share the ring, so the
// scrollback is at most what the view leaves over. Other rows are in view
// now, and one of them may be owed a fill from before it left.
void screen_fit(screen_t *s, uint8_t height)
{
    screen_ring_fit(s, height);
    if (s->cells)
        s->cells->all_clean = false;
}

void screen_reset(screen_t *s, uint8_t height)
{
    if (!s->cells)
    {
        screen_ring_clear(s);
        return;
    }
    const screen_fill_t e = screen_blank(' ', MODE0_ENTRY(MODE0_DEFAULT_FG, false),
                                         MODE0_ENTRY(MODE0_DEFAULT_BG, false), false);
    screen_mark_erased(s, height, &e);
}

bool screen_row_used(screen_t *s, uint8_t y)
{
    if (!screen_settled(s))
        return false;
    screen_clean_line(s, y);
    const mode0_cell_t *row = screen_row(s, y);
    const mode0_colors_t *colors = screen_cell_colors(row);
    const uint16_t blank = MODE0_ENTRY(MODE0_DEFAULT_BG, false);
    for (unsigned x = 0; x < s->cells->width; x++)
        if (mode0_glyph_code(row[x].glyph) != ' ' ||
            (row[x].attributes & MODE0_ATTR_LINES) ||
            colors[x].bg_color != blank)
            return true;
    return false;
}

void screen_blank_cells(mode0_cell_t *cells, unsigned n)
{
    const screen_fill_t e = screen_blank(' ', MODE0_ENTRY(MODE0_DEFAULT_FG, false),
                                         MODE0_ENTRY(MODE0_DEFAULT_BG, false), false);
    screen_run_fill(cells, n, e.cell, e.colors);
}

#endif /* TERM_CELLS */
