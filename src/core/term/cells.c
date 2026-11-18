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

// Runs of cells. A cell is a word, and its colors are three halfwords in
// the table beside it (mode0.h), which go wherever the cell goes.
// Everything that writes or moves more than a cell does it here.

static inline uint32_t cells_at(const mode0_cell_t *cell)
{
    return (uint32_t)(cell - mode0_cells);
}

// The colors of a cell.
static inline mode0_colors_t *cells_colors(const mode0_cell_t *cell)
{
    return &mode0_colors[cells_at(cell)];
}

// GCC turns a loop that copies or fills into a call of memmove or memset,
// and the C library's are slower here, a byte at a time.
#if defined(__GNUC__) && !defined(__clang__)
#define CELLS_RUN __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define CELLS_RUN
#endif

// The n cells at dst become `cell`, and their colors `colors`. The cells
// are written eight a pass. Two cells' colors are three words, so the
// colors are written four cells a pass from the first that is on a word.
static CELLS_RUN void cells_run_fill(mode0_cell_t *dst, unsigned n, mode0_cell_t cell,
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
    mode0_colors_t *c = cells_colors(dst);
    i = n;
    if (i && (cells_at(dst) & 1))
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
cells_words_move(mode_word_t *d, const mode_word_t *f, unsigned w)
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
static CELLS_RUN void cells_run_move(mode0_cell_t *dst, const mode0_cell_t *src, unsigned n)
{
    cells_words_move((mode_word_t *)dst, (const mode_word_t *)src, n);
    uint16_t *cd = (uint16_t *)cells_colors(dst);
    const uint16_t *cf = (const uint16_t *)cells_colors(src);
    unsigned halves = 3 * n;
    if (halves && (cells_at(dst) & cells_at(src) & 1))
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
            cells_words_move((mode_word_t *)(cd + 1), (const mode_word_t *)(cf + 1), halves / 2);
            cd[0] = cf[0];
            return;
        }
    }
    if ((halves & 1) && dst > src)
        cd[halves - 1] = cf[halves - 1];
    cells_words_move((mode_word_t *)cd, (const mode_word_t *)cf, halves / 2);
    if ((halves & 1) && dst < src)
        cd[halves - 1] = cf[halves - 1];
}

// A fill owed to a row: the cell every cell of the row becomes, and its
// colors. An erase is a fill with a blank cell, DECALN's is a fill with an
// E, and REP's is a fill with its glyph as the pen draws it. A selective
// erase leaves the guarded cells alone.
typedef struct
{
    mode0_cell_t cell;
    mode0_colors_t colors;
    bool selective;
} cells_fill_t;

// A shift owed to a row, SL's and SR's: the cells that survive it, where
// they land, and the blanks either side of them. The survivors are the
// cells that land on columns [to, end), and the first of them is at column
// from until the row is paid. The columns left of to are blank in the left
// colors and those from end to cols in the right colors. Another shift of
// the row only changes the record.
typedef struct
{
    /* On a word, so the record is copied a word at a time. */
    _Alignas(4) uint8_t from;
    uint8_t to;
    uint8_t end;
    uint8_t cols;
    uint16_t left_fg;
    uint16_t left_bg;
    uint16_t right_fg;
    uint16_t right_bg;
} cells_shift_t;

// A change of attributes owed to a row, DECCARA's and DECRARA's: the
// columns [from, to), and the CELLS_MARK_ attributes to set, to clear, and
// then to reverse.
typedef struct
{
    _Alignas(4) uint8_t from;
    uint8_t to;
    uint8_t set;
    uint8_t clear;
    uint8_t toggle;
} cells_marks_t;

// What a row is owed: nothing, a fill, a shift, or a change of attributes.
enum
{
    CELLS_OWED_NONE,
    CELLS_OWED_FILL,
    CELLS_OWED_SHIFT,
    CELLS_OWED_MARKS,
};

typedef union
{
    cells_fill_t fill;
    cells_shift_t shift;
    cells_marks_t marks;
} cells_owed_t;

// A scroll between left and right margins owed to a screen: columns
// [from, end) of rows [top, bot] move n rows up or down, and this blank
// comes in. n is 0 when no scroll is owed. Another scroll of the same
// columns the same way only adds to n, so a run of line feeds moves the
// cells once.
typedef struct
{
    uint8_t top, bot, from, end;
    uint8_t n;
    bool up;
    cells_fill_t in;
} cells_later_t;

// The cells of a screen and how its rows are laid out: the scroll remap and
// what the rows are still owed. A row of the screen is in a row of memory,
// which row_idx gives by slot. A fill or a shift is owed to the row of
// memory, so it goes where the row goes and a scroll moves none of them.
typedef struct
{
    mode0_cell_t *mem;
    uint8_t width;
    uint8_t row_idx[TERM_MAX_HEIGHT];
    uint8_t owed[TERM_MAX_HEIGHT];
    cells_owed_t what[TERM_MAX_HEIGHT];
    cells_later_t later;
    uint8_t y_offset;
    /* The rows scrolled off the top that are still in the cell store, the
     * newest first. A taller view shows them again; ED 3 forgets them. */
    uint8_t hidden;
    bool all_clean;
} screen_buf_t;

// The terminal at one size and its screens' cells.
typedef struct
{
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
    return g == &cells_80.grid ? &cells_80 : g == &cells_40.grid ? &cells_40 : NULL;
}

// The screen showing, which the terminal chooses.
static inline screen_buf_t *cells_buf(cells_t *cells)
{
    return &cells->bufs[cells_term.alt_active ? 1 : 0];
}

static void cells_settle(screen_buf_t *buf);

// The cells of a grid, with the scroll its screen is owed done: for
// everything that changes a cell.
static inline cells_t *cells_settled(term_grid_t *g)
{
    cells_t *cells = cells_of(g);
    if (cells && cells_buf(cells)->later.n)
        cells_settle(cells_buf(cells));
    return cells;
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

// The row of memory that row y of the screen is in.
static inline uint8_t cells_buf_mem(const screen_buf_t *buf, uint8_t y)
{
    return buf->row_idx[cells_buf_slot(buf->y_offset, y)];
}

static inline mode0_cell_t *cells_buf_row(const screen_buf_t *buf, uint8_t y)
{
    return buf->mem + (uint32_t)cells_buf_mem(buf, y) * buf->width;
}

// Translate a logical row (0..height-1) into the start of its physical
// cell row.
static inline mode0_cell_t *cells_row_ptr(cells_t *cells, uint8_t y)
{
    return cells_buf_row(cells_buf(cells), y);
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

// Fill [start, end) of `row`: every cell becomes the fill's cell, in its
// colors. A selective erase leaves the guarded cells as they are.
static void fill_glyph(mode0_cell_t *row, unsigned start, unsigned end,
                       const cells_fill_t *e)
{
    if (!e->selective)
    {
        cells_run_fill(row + start, end - start, e->cell, e->colors);
        return;
    }
    mode0_colors_t *colors = cells_colors(row);
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
static cells_fill_t cells_blank(uint8_t glyph, uint16_t fg, uint16_t bg, bool selective)
{
    const cells_fill_t e = {{glyph, 0}, {fg, bg, fg}, selective};
    return e;
}

// Two fills that leave a row the same.
static inline bool cells_fill_same(const cells_fill_t *a, const cells_fill_t *b)
{
    return a->selective == b->selective && a->cell.glyph == b->cell.glyph &&
           a->cell.attributes == b->cell.attributes &&
           a->colors.fg_color == b->colors.fg_color &&
           a->colors.bg_color == b->colors.bg_color &&
           a->colors.ul_color == b->colors.ul_color;
}

// An erase in the pen's colors, selective or not. An erase takes the pen's
// colors, not reversed, and none of its attributes, as xterm and Linux do.
static cells_fill_t cells_pen_erase(const term_t *term, bool selective)
{
    const term_pen_t *pen = term_pen(term);
    return cells_blank(' ', cells_pen_fg(pen), cells_pen_bg(term, pen), selective);
}

// Erase [start, end) of `row` in the pen's colors.
static void cells_fill_erased(term_t *term, mode0_cell_t *row, unsigned start, unsigned end,
                              bool selective)
{
    const cells_fill_t e = cells_pen_erase(term, selective);
    fill_glyph(row, start, end, &e);
}

// Bold on or off for a cell's ink, which is a palette entry it can be told
// of: on, entries 0 to 7 and the default foreground become their bright
// entries, as the pen draws them; off, an ink that bold brightened goes
// back. An RGB ink, or one that was bright without bold, stays as it is.
// Returns the cell's attributes.
static uint16_t cells_ink_bold(uint16_t *ink, uint16_t attributes, bool on)
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
static void cells_row_marks(mode0_cell_t *row, unsigned from, unsigned to,
                            unsigned set, unsigned clear, unsigned toggle)
{
    mode0_colors_t *colors = cells_colors(row);
    for (unsigned i = from; i < to; i++)
    {
        uint16_t a = row[i].attributes;
        mode0_colors_t *c = &colors[i];
        const unsigned has = ((a & MODE0_ATTR_BOLD) ? CELLS_MARK_BOLD : 0) |
                             ((a & TERM_ATTR_UL_MASK) ? CELLS_MARK_UNDERLINE : 0) |
                             ((a & TERM_ATTR_ANY_BLINK) ? CELLS_MARK_BLINK : 0) |
                             ((a & MODE0_ATTR_REVERSE) ? CELLS_MARK_REVERSE : 0);
        const unsigned want = ((has | set) & ~clear) ^ toggle;
        const unsigned change = has ^ want;
        if (!change)
            continue;
        if (change & CELLS_MARK_REVERSE)
        {
            const uint16_t fg = c->fg_color;
            c->fg_color = c->bg_color;
            c->bg_color = fg;
            a ^= MODE0_ATTR_REVERSE;
        }
        if ((change & CELLS_MARK_BOLD) && c->fg_color != c->bg_color)
            a = cells_ink_bold((a & MODE0_ATTR_REVERSE) ? &c->bg_color : &c->fg_color, a,
                               want & CELLS_MARK_BOLD);
        if (change & CELLS_MARK_UNDERLINE)
        {
            a &= (uint16_t)~(TERM_ATTR_UL_MASK | MODE0_ATTR_UL_COLOR);
            if (want & CELLS_MARK_UNDERLINE)
                a |= TERM_ATTR_UL(TERM_UL_SINGLE);
        }
        if (change & CELLS_MARK_BLINK)
        {
            a &= (uint16_t)~TERM_ATTR_ANY_BLINK;
            if (want & CELLS_MARK_BLINK)
                a |= TERM_ATTR_BLINK;
        }
        if (!(a & MODE0_ATTR_UL_COLOR))
            c->ul_color = c->fg_color;
        row[i].attributes = a;
    }
}

// Pay row m of memory what it is owed. A shift moves the survivors, then
// blanks the columns either side of where they landed.
static void cells_pay(screen_buf_t *buf, uint8_t m)
{
    mode0_cell_t *row = buf->mem + (uint32_t)m * buf->width;
    const uint8_t owed = buf->owed[m];
    buf->owed[m] = CELLS_OWED_NONE;
    if (owed == CELLS_OWED_FILL)
        fill_glyph(row, 0, buf->width, &buf->what[m].fill);
    else if (owed == CELLS_OWED_MARKS)
    {
        const cells_marks_t *k = &buf->what[m].marks;
        cells_row_marks(row, k->from, k->to, k->set, k->clear, k->toggle);
    }
    else if (owed == CELLS_OWED_SHIFT)
    {
        const cells_shift_t *s = &buf->what[m].shift;
        cells_run_move(row + s->to, row + s->from, (unsigned)(s->end - s->to));
        if (s->to)
        {
            const cells_fill_t e = cells_blank(' ', s->left_fg, s->left_bg, false);
            cells_run_fill(row, s->to, e.cell, e.colors);
        }
        if (s->end < s->cols)
        {
            const cells_fill_t e = cells_blank(' ', s->right_fg, s->right_bg, false);
            cells_run_fill(row + s->end, (unsigned)(s->cols - s->end), e.cell, e.colors);
        }
    }
}

// Make sure you call this any time you change rows.
// It pays the row what it is still owed.
static void cells_clean_line(cells_t *cells, uint8_t y)
{
    screen_buf_t *buf = cells_buf(cells);
    const uint8_t m = cells_buf_mem(buf, y);
    if (buf->owed[m])
        cells_pay(buf, m);
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
static bool cells_clean_one_dirty(screen_buf_t *buf, uint8_t height)
{
    if (buf->all_clean)
        return false;
    for (uint8_t i = 0; i < height; i++)
    {
        const uint8_t m = cells_buf_mem(buf, i);
        if (buf->owed[m])
        {
            cells_pay(buf, m);
            return true;
        }
    }
    buf->all_clean = true;
    return false;
}

// Do a scroll a screen is owed, or else clean at most one row per tick:
// prefer the active buffer; only touch the inactive one if the active is
// already clean. Invariant: the cursor's
// row is never dirty on the active buffer (the terminal touches every row it
// puts the cursor on), so the cursor cell is never affected by this task and
// it never has to call cells_cursor_restart_blink.
static void cells_clean_task(cells_t *cells)
{
    for (int b = 0; b < 2; b++)
        if (cells->bufs[b].later.n)
        {
            cells_settle(&cells->bufs[b]);
            return;
        }
    if (cells_clean_one_dirty(cells_buf(cells), cells->grid.height))
        return;
    screen_buf_t *other = &cells->bufs[cells_term.alt_active ? 0 : 1];
    if (other->mem)
        cells_clean_one_dirty(other, cells->grid.height);
}

// Rows [from, to) of the screen are owed this fill, in place of whatever
// they were owed. A row owed a whole fill has every cell guarded or none,
// so a selective erase of it spares all of that fill or lands whole. A row
// owed a shift or a change of attributes is paid before a selective erase,
// which goes by its guards.
static void cells_mark_rows_with(screen_buf_t *s, uint8_t from, uint8_t to,
                                 const cells_fill_t *e)
{
    for (uint8_t i = from; i < to; i++)
    {
        const uint8_t m = cells_buf_mem(s, i);
        cells_fill_t *fill = &s->what[m].fill;
        if (e->selective && s->owed[m] >= CELLS_OWED_SHIFT)
            cells_pay(s, m);
        else if (e->selective && s->owed[m] == CELLS_OWED_FILL && !fill->selective)
        {
            if (fill->cell.attributes & MODE0_ATTR_GUARD)
                continue;
            *fill = *e;
            fill->selective = false;
            continue;
        }
        s->owed[m] = CELLS_OWED_FILL;
        *fill = *e;
    }
    s->all_clean = false;
}

// Lazy-erase rows [from, to) to spaces in the default colors.
static void cells_mark_rows_default(screen_buf_t *s, uint8_t from, uint8_t to)
{
    const cells_fill_t e = cells_blank(' ', MODE0_ENTRY(MODE0_DEFAULT_FG, false),
                                       MODE0_ENTRY(MODE0_DEFAULT_BG, false), false);
    cells_mark_rows_with(s, from, to, &e);
}

// Lazy-erase rows [from, to) of the active screen in the pen's colors.
static void cells_mark_rows_erase(term_t *term, cells_t *cells, uint8_t from, uint8_t to,
                                  bool selective)
{
    const cells_fill_t e = cells_pen_erase(term, selective);
    cells_mark_rows_with(cells_buf(cells), from, to, &e);
}

// Lazy clear of the given screen buffer: resets y_offset / row_idx to
// identity and marks every visible row dirty with the given erase. A scroll
// the screen was owed is forgotten with the cells it would have moved.
// No cell memory is touched here -- cells_clean_task drains dirty rows in
// the background (one row per tick, on both buffers), and the active
// buffer's cursor row is also cleaned synchronously when the terminal
// touches it.
static void cells_mark_buf_erased(screen_buf_t *buf, uint8_t height, const cells_fill_t *e)
{
    buf->y_offset = 0;
    buf->hidden = 0;
    buf->later.n = 0;
    for (uint8_t y = 0; y < TERM_MAX_HEIGHT; y++)
    {
        buf->row_idx[y] = y;
        buf->owed[y] = CELLS_OWED_NONE;
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

// The cell a glyph lands as on a grid: the pen's colors and attributes,
// through the active character set. The cell holds where the font has the
// glyph (mode0.h): DEC Special Graphics for that set's 0x5F to 0x7E, the
// italics for SGR 3's ASCII, the code page's otherwise. The UK set's pound
// sign is the DEC set's 0x7D. SGR 7 and 8 swap and hide at the cell, and in
// iCE colors mode (?33) the blink bits are left off the cell: the bright
// background is the blink. An underline SGR 58 colored is flagged, so the
// renderer reads its color; any other holds the foreground's color. A cell
// written while DECSCA or SPA protects is guarded. The cell says when bold
// brightened its ink and when its colors are swapped, so DECCARA and
// DECRARA can undo either; a concealed cell has no ink to say it of.
static cells_fill_t cells_glyph_cell(term_t *term, const term_grid_t *g, uint8_t byte)
{
    const term_pen_t *pen = term_pen(term);
    const unsigned rows = term_cell_height(g);
    cells_fill_t e;
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
    uint8_t set = pen->gl_is_g1 ? pen->g1_charset : pen->g0_charset;
    if (set == TERM_CS_DEC && byte >= 0x5F && byte <= 0x7E)
        glyph = MODE0_GLYPH_DEC(rows, byte);
    else if (set == TERM_CS_UK && byte == '#')
        glyph = MODE0_GLYPH_DEC(rows, 0x7D);
    else if ((pen->sgr_attr & TERM_ATTR_ITALIC) && byte < 0x80)
        glyph = MODE0_GLYPH_ITALIC(rows, byte);
    e.cell.glyph = glyph;
    e.cell.attributes = attributes;
    e.colors.ul_color = pen->ul_set ? cells_base(&pen->ul, MODE0_DEFAULT_FG) : f;
    e.selective = false;
    return e;
}

void cells_glyphs(term_t *term, term_grid_t *g, uint8_t y, uint8_t x, uint8_t byte, unsigned n)
{
    if (!cells_settled(g))
        return;
    const cells_fill_t e = cells_glyph_cell(term, g, byte);
    mode0_cell_t *at = cells_row_ptr(cells_of(g), y) + x;
    if (n == 1)
    {
        // A character: its word and its three colors.
        *at = e.cell;
        *cells_colors(at) = e.colors;
        return;
    }
    cells_run_fill(at, n, e.cell, e.colors);
}

void cells_fill_row(term_t *term, term_grid_t *g, uint8_t y, uint8_t byte)
{
    if (!cells_settled(g))
        return;
    const cells_fill_t e = cells_glyph_cell(term, g, byte);
    cells_mark_rows_with(cells_buf(cells_of(g)), y, (uint8_t)(y + 1), &e);
}

void cells_erase(term_t *term, term_grid_t *g, uint8_t y, unsigned from, unsigned to,
                 bool selective)
{
    if (!cells_settled(g))
        return;
    cells_fill_erased(term, cells_row_ptr(cells_of(g), y), from, to, selective);
}

// Columns [x, end) of a row move n right, erased cells coming in at x; or n
// left, erased cells coming in before end.
static void cells_row_insert(term_t *term, mode0_cell_t *row, unsigned x, unsigned end,
                             unsigned n)
{
    cells_run_move(&row[x + n], &row[x], end - x - n);
    cells_fill_erased(term, row, x, x + n, false);
}

static void cells_row_delete(term_t *term, mode0_cell_t *row, unsigned x, unsigned end,
                             unsigned n)
{
    cells_run_move(&row[x], &row[x + n], end - x - n);
    cells_fill_erased(term, row, end - n, end, false);
}

void cells_insert(term_t *term, term_grid_t *g, uint8_t y, uint8_t x, unsigned end, unsigned n)
{
    if (!cells_settled(g))
        return;
    cells_row_insert(term, cells_row_ptr(cells_of(g), y), x, end, n);
}

void cells_delete(term_t *term, term_grid_t *g, uint8_t y, uint8_t x, unsigned end, unsigned n)
{
    if (!cells_settled(g))
        return;
    cells_row_delete(term, cells_row_ptr(cells_of(g), y), x, end, n);
}

void cells_copy(term_t *term, term_grid_t *g, uint8_t sy, unsigned sx, uint8_t dy, unsigned dx,
                unsigned n)
{
    if (!cells_settled(g))
        return;
    (void)term;
    cells_t *cells = cells_of(g);
    cells_clean_line(cells, sy);
    cells_clean_line(cells, dy);
    cells_run_move(cells_row_ptr(cells, dy) + dx, cells_row_ptr(cells, sy) + sx, n);
}

// The row is only owed the change, after whatever it was owed before: no
// cell changes until the row is paid. The cursor's row is never left owing.
// In iCE colors mode (?33) no cell blinks, so none is made to.
void cells_attrs(term_t *term, term_grid_t *g, uint8_t y, unsigned from, unsigned to,
                 unsigned set, unsigned clear, unsigned toggle)
{
    if (!cells_settled(g))
        return;
    cells_t *cells = cells_of(g);
    screen_buf_t *buf = cells_buf(cells);
    const uint8_t m = cells_buf_mem(buf, y);
    if (term->ice_colors)
    {
        set &= ~CELLS_MARK_BLINK;
        toggle &= ~CELLS_MARK_BLINK;
    }
    if (buf->owed[m])
        cells_pay(buf, m);
    const cells_marks_t marks = {(uint8_t)from, (uint8_t)to, (uint8_t)set, (uint8_t)clear,
                                 (uint8_t)toggle};
    buf->what[m].marks = marks;
    buf->owed[m] = CELLS_OWED_MARKS;
    buf->all_clean = false;
    if (y == term_pos(g)->y)
        cells_pay(buf, m);
}

// One more shift of n columns, with blanks in these colors, added to the
// shift a row of cols columns is owed.
static inline void cells_shift_add(cells_shift_t *s, uint8_t cols, uint8_t n, bool left,
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
void cells_shift(term_t *term, term_grid_t *g, uint8_t top, uint8_t bot, unsigned from,
                 unsigned end, unsigned count, bool left)
{
    if (!cells_settled(g))
        return;
    cells_t *cells = cells_of(g);
    screen_buf_t *buf = cells_buf(cells);
    if (from || end != buf->width)
    {
        for (uint8_t y = top; y <= bot; y++)
        {
            const unsigned cols = term_cols(g, y);
            const unsigned e = end < cols ? end : cols;
            if (from >= e)
                continue;
            const unsigned n = count < e - from ? count : e - from;
            cells_clean_line(cells, y);
            if (left)
                cells_row_delete(term, cells_row_ptr(cells, y), from, e, n);
            else
                cells_row_insert(term, cells_row_ptr(cells, y), from, e, n);
        }
        return;
    }
    const cells_fill_t in = cells_pen_erase(term, false);
    const uint16_t fg = in.colors.fg_color, bg = in.colors.bg_color;
    const uint8_t width = buf->width;
    const uint8_t *lines = term_lines(g);
    cells_shift_t whole = {0, 0, width, width, fg, bg, fg, bg};
    cells_shift_add(&whole, width, count < width ? (uint8_t)count : width, left, fg, bg);
    for (uint8_t y = top; y <= bot; y++)
    {
        const uint8_t m = cells_buf_mem(buf, y);
        cells_shift_t *s = &buf->what[m].shift;
        const uint8_t owed = buf->owed[m];
        if (owed == CELLS_OWED_NONE && !lines[y])
        {
            buf->owed[m] = CELLS_OWED_SHIFT;
            *s = whole;
            continue;
        }
        const uint8_t cols = lines[y] ? width / 2u : width;
        const uint8_t n = count < cols ? (uint8_t)count : cols;
        cells_shift_t r = {0, 0, cols, cols, fg, bg, fg, bg};
        if (owed == CELLS_OWED_FILL)
        {
            const cells_fill_t *fill = &buf->what[m].fill;
            if (cells_fill_same(fill, &in))
                continue;
            cells_pay(buf, m);
        }
        else if (owed == CELLS_OWED_SHIFT)
        {
            if (n < cols && (left ? s->end < cols && (s->right_fg != fg || s->right_bg != bg)
                                  : s->to && (s->left_fg != fg || s->left_bg != bg)))
                cells_pay(buf, m);
            else
                r = *s;
        }
        else if (owed == CELLS_OWED_MARKS)
            cells_pay(buf, m);
        cells_shift_add(&r, cols, n, left, fg, bg);
        buf->owed[m] = CELLS_OWED_SHIFT;
        *s = r;
    }
    buf->all_clean = false;
    cells_clean_line(cells, term_pos(g)->y);
}

void cells_erase_rows(term_t *term, term_grid_t *g, uint8_t from, uint8_t to,
                      bool selective)
{
    if (!cells_settled(g))
        return;
    cells_mark_rows_erase(term, cells_of(g), from, to, selective);
}

void cells_align(term_t *term, term_grid_t *g)
{
    if (!cells_settled(g))
        return;
    (void)term;
    const cells_fill_t e = cells_blank('E', MODE0_ENTRY(MODE0_DEFAULT_FG, false),
                                       MODE0_ENTRY(MODE0_DEFAULT_BG, false), false);
    cells_mark_rows_with(cells_buf(cells_of(g)), 0, g->height, &e);
}

// Rotate row_idx within rows [top, bot] by n: each row takes the row of
// memory n rows below it, or above it, and the n at the end go round. What
// a row is owed is its row of memory's, so it goes with it.
static void cells_rows_rotate(screen_buf_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up)
{
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
    }
    else
    {
        for (uint8_t i = 0; i < n; i++)
            saved[i] = s->row_idx[cells_buf_slot(y_off, (uint8_t)(bot - n + 1 + i))];
        for (int i = (int)bot; i - (int)n >= (int)top; i--)
            s->row_idx[cells_buf_slot(y_off, (uint8_t)i)] = s->row_idx[cells_buf_slot(y_off, (uint8_t)(i - n))];
        for (uint8_t i = 0; i < n; i++)
            s->row_idx[cells_buf_slot(y_off, (uint8_t)(top + i))] = saved[i];
    }
}

// The rows move, and the n rows coming in are lazy-erased in the pen's
// colors.
void cells_scroll(term_t *term, term_grid_t *g, uint8_t top, uint8_t bot, uint8_t n, bool up)
{
    if (!cells_settled(g))
        return;
    cells_t *cells = cells_of(g);
    cells_rows_rotate(cells_buf(cells), top, bot, n, up);
    if (up)
        cells_mark_rows_erase(term, cells, (uint8_t)(bot - n + 1), (uint8_t)(bot + 1), false);
    else
        cells_mark_rows_erase(term, cells, top, (uint8_t)(top + n), false);
}

// The cells of a row either side of columns [from, end), kept while the
// rows move round. No more than this many are ever kept: with more either
// side than between, the cells between are the ones copied.
#define CELLS_STRIP_MAX 40
typedef struct
{
    mode0_cell_t cell[CELLS_STRIP_MAX];
    mode0_colors_t colors[CELLS_STRIP_MAX];
} cells_strip_t;

static CELLS_RUN void cells_strip_keep(cells_strip_t *strip, const mode0_cell_t *row,
                                       unsigned from, unsigned end, unsigned width)
{
    const mode0_colors_t *colors = cells_colors(row);
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

static CELLS_RUN void cells_strip_put(const cells_strip_t *strip, mode0_cell_t *row,
                                      unsigned from, unsigned end, unsigned width)
{
    mode0_colors_t *colors = cells_colors(row);
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

static CELLS_RUN void cells_strip_move(mode0_cell_t *to, const mode0_cell_t *row,
                                       unsigned from, unsigned end, unsigned width)
{
    mode0_colors_t *tc = cells_colors(to);
    const mode0_colors_t *rc = cells_colors(row);
    if (from > 4)
        cells_run_move(to, row, from);
    else
        for (unsigned i = 0; i < from; i++)
        {
            to[i] = row[i];
            tc[i] = rc[i];
        }
    if (width - end > 4)
        cells_run_move(to + end, row + end, width - end);
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
// cells either side go back to the rows they were on, each row's to the
// next round the region. No row of the region is owed anything: they were
// paid when the scroll came to be owed, and everything that changes a cell
// does the scroll first.
static void cells_settle(screen_buf_t *buf)
{
    const cells_later_t l = buf->later;
    if (!l.n)
        return;
    buf->later.n = 0;
    cells_stale = true;
    const unsigned top = l.top, bot = l.bot, from = l.from, end = l.end, n = l.n;
    const unsigned h = bot - top + 1;
    const unsigned width = buf->width, inside = end - from, outside = width - inside;
    if (n < h && (outside > CELLS_STRIP_MAX || inside * (h - n) <= outside * h))
    {
        for (unsigned i = 0; i < h - n; i++)
        {
            const unsigned to = l.up ? top + i : bot - i;
            cells_run_move(cells_buf_row(buf, (uint8_t)to) + from,
                           cells_buf_row(buf, (uint8_t)(l.up ? to + n : to - n)) + from, inside);
        }
    }
    else if (n < h)
    {
        cells_rows_rotate(buf, (uint8_t)top, (uint8_t)bot, (uint8_t)n, l.up);
        // Row j of the region now has the cells either side that belong
        // on row j + step, counted round the region.
        const unsigned step = l.up ? n : h - n;
        cells_strip_t strip;
        for (unsigned first = 0, moved = 0; moved < h; first++)
        {
            mode0_cell_t *row = cells_buf_row(buf, (uint8_t)(top + first));
            cells_strip_keep(&strip, row, from, end, width);
            for (unsigned j = first;;)
            {
                unsigned src = j >= step ? j - step : j + h - step;
                moved++;
                if (src == first)
                {
                    cells_strip_put(&strip, row, from, end, width);
                    break;
                }
                mode0_cell_t *other = cells_buf_row(buf, (uint8_t)(top + src));
                cells_strip_move(row, other, from, end, width);
                row = other;
                j = src;
            }
        }
    }
    for (unsigned i = 0; i < n; i++)
        fill_glyph(cells_buf_row(buf, (uint8_t)(l.up ? bot - i : top + i)), from, end, &l.in);
}

// The screen is only owed the scroll. One of the same columns the same way
// that it already owes is added to; any other is done first. Every row of
// the region is paid what it is owed, which is owed to a whole row.
void cells_scroll_cols(term_t *term, term_grid_t *g, uint8_t top, uint8_t bot, unsigned from,
                       unsigned end, uint8_t n, bool up)
{
    if (!cells_of(g))
        return;
    cells_t *cells = cells_of(g);
    screen_buf_t *buf = cells_buf(cells);
    cells_later_t *l = &buf->later;
    const cells_fill_t in = cells_pen_erase(term, false);
    const unsigned h = (unsigned)bot - top + 1;
    if (l->n && l->top == top && l->bot == bot && l->from == from && l->end == end &&
        l->up == up && cells_fill_same(&l->in, &in))
    {
        l->n = (uint8_t)(l->n + n < h ? l->n + n : h);
        return;
    }
    cells_settle(buf);
    for (unsigned y = top; y <= bot; y++)
        cells_clean_line(cells, (uint8_t)y);
    const cells_later_t later = {top, bot, (uint8_t)from, (uint8_t)end, n, up, in};
    *l = later;
}

// The fast scroll: the viewport rotates through y_offset.
void cells_index(term_t *term, term_grid_t *g, bool up)
{
    if (!cells_settled(g))
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
        cells_mark_rows_erase(term, cells, last, g->height, false);
    }
    else
    {
        s->y_offset = s->y_offset ? (uint8_t)(s->y_offset - 1) : (uint8_t)(TERM_MAX_HEIGHT - 1);
        if (s->hidden)
            s->hidden--;
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
    const cells_fill_t e = cells_pen_erase(term, false);
    cells_mark_buf_erased(cells_buf(cells_of(g)), g->height, &e);
}

void cells_height(term_grid_t *g, bool grow, bool top)
{
    if (!cells_settled(g))
        return;
    cells_t *cells = cells_of(g);
    screen_buf_t *s = cells_buf(cells);
    uint8_t h = g->height;
    // Other rows are in view now, and one of them may be owed a fill from
    // before it left.
    cells->bufs[0].all_clean = cells->bufs[1].all_clean = false;
    if (grow && top)
    {
        // Everything moves down a row and the row above comes into view. It
        // is a row of scrollback when there is one, otherwise a blank one.
        if (!s->y_offset)
            s->y_offset = TERM_MAX_HEIGHT - 1;
        else
            s->y_offset--;
        // A row of scrollback comes back as it left, with the fill it was
        // still owed.
        if (s->hidden)
            s->hidden--;
        else
            cells_mark_rows_default(s, 0, 1);
        // The cursor's row is another row now, and is never owed a fill.
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
        const cells_fill_t e = cells_blank(' ', MODE0_ENTRY(MODE0_DEFAULT_FG, false),
                                           MODE0_ENTRY(MODE0_DEFAULT_BG, false), false);
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

// The layout the row words were last written from: the screen showing, and
// which row of memory and what line size each of its rows has. Only a
// scroll, a line size, a change of height, or another screen showing
// changes it, so most bytes leave the row words as they are.
static struct
{
    const screen_buf_t *buf;
    uint8_t y_offset;
    uint8_t height;
    uint8_t row_idx[TERM_MAX_HEIGHT];
    uint8_t lines[TERM_MAX_HEIGHT];
} cells_published;

// TERM_MAX_HEIGHT bytes compared a word at a time.
static inline bool cells_rows_differ(const uint8_t *a, const uint8_t *b)
{
    uint32_t diff = 0;
    unsigned i = 0;
    for (; i + 4 <= TERM_MAX_HEIGHT; i += 4)
    {
        uint32_t x, y;
        memcpy(&x, a + i, 4);
        memcpy(&y, b + i, 4);
        diff |= x ^ y;
    }
    for (; i < TERM_MAX_HEIGHT; i++)
        diff |= (uint32_t)(a[i] ^ b[i]);
    return diff != 0;
}

void cells_publish(void)
{
    cells_t *cells = cells_visible();
    const screen_buf_t *buf = cells_buf(cells);
    const uint8_t *ls = term_lines(&cells->grid);
    if (cells_published.buf != buf || cells_published.y_offset != buf->y_offset ||
        cells_published.height != cells->grid.height ||
        cells_rows_differ(cells_published.row_idx, buf->row_idx) ||
        cells_rows_differ(cells_published.lines, ls))
    {
        cells_published.buf = buf;
        cells_published.y_offset = buf->y_offset;
        cells_published.height = cells->grid.height;
        memcpy(cells_published.row_idx, buf->row_idx, TERM_MAX_HEIGHT);
        memcpy(cells_published.lines, ls, TERM_MAX_HEIGHT);
        for (uint8_t y = 0; y < cells->grid.height; y++)
            mode0_regs.row[y] = MODE0_ROW(cells_buf_row(buf, y) - mode0_cells, ls[y]);
    }
    const term_pos_t *pos = term_pos(&cells->grid);
    // A cursor with a wrap pending is on the column it wrote, as on a VT100.
    mode0_regs.cursor = MODE0_CURSOR(pos->x, pos->y, cells_term.cursor_style) |
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
    cells_40.bufs[0].width = cells_40.bufs[1].width = 40;
    cells_80.bufs[0].width = cells_80.bufs[1].width = 80;
    term_grid_init(&cells_40.grid, 40, cells_40.lines, TERM_MAX_HEIGHT);
    term_grid_init(&cells_80.grid, 80, cells_80.lines, TERM_MAX_HEIGHT);
    term_grid_t *const grids[] = {&cells_40.grid, &cells_80.grid};
    term_init(&cells_term, grids, 2);
    // become part of stdout
    manifold_set_term_out(cells_out_chars);
}

// Cursor blink timing only — toggles cursor_lit on its cadence and forces
// unlit when the cursor is disabled. Steady-vs-blink handling lives in the renderers,
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
        cells->cursor_frame = vga_frame_count() + CELLS_CURSOR_BLINK_FRAMES;
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
    /* What each row is owed, and its record in 12 bytes. */
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
    {
        sst_put_u8(c, b->owed[i]);
        if (b->owed[i] == CELLS_OWED_SHIFT)
        {
            const cells_shift_t *s = &b->what[i].shift;
            sst_put_u8(c, s->from);
            sst_put_u8(c, s->to);
            sst_put_u8(c, s->end);
            sst_put_u8(c, s->cols);
            sst_put_u16(c, s->left_fg);
            sst_put_u16(c, s->left_bg);
            sst_put_u16(c, s->right_fg);
            sst_put_u16(c, s->right_bg);
        }
        else if (b->owed[i] == CELLS_OWED_MARKS)
        {
            const cells_marks_t *k = &b->what[i].marks;
            sst_put_u8(c, k->from);
            sst_put_u8(c, k->to);
            sst_put_u8(c, k->set);
            sst_put_u8(c, k->clear);
            sst_put_u8(c, k->toggle);
            for (int n = 5; n < 12; n++)
                sst_put_u8(c, 0);
        }
        else if (b->owed[i] == CELLS_OWED_FILL)
        {
            const cells_fill_t *e = &b->what[i].fill;
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
    sst_put_u8(c, b->y_offset);
    sst_put_u8(c, b->hidden);
    sst_put_bool(c, b->all_clean);
}

static bool cells_get_buf(sst_cursor_t *c, screen_buf_t *b)
{
    bool ok = true;
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        b->row_idx[i] = sst_get_u8(c);
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
    {
        b->owed[i] = sst_get_u8(c);
        memset(&b->what[i], 0, sizeof(b->what[i]));
        if (b->owed[i] == CELLS_OWED_SHIFT)
        {
            cells_shift_t *s = &b->what[i].shift;
            s->from = sst_get_u8(c);
            s->to = sst_get_u8(c);
            s->end = sst_get_u8(c);
            s->cols = sst_get_u8(c);
            s->left_fg = sst_get_u16(c);
            s->left_bg = sst_get_u16(c);
            s->right_fg = sst_get_u16(c);
            s->right_bg = sst_get_u16(c);
            /* The record indexes the row's cells. */
            ok &= s->cols <= b->width && s->end <= s->cols && s->to <= s->end &&
                  s->from + s->end - s->to <= s->cols &&
                  cells_color_ok(s->left_fg) && cells_color_ok(s->left_bg) &&
                  cells_color_ok(s->right_fg) && cells_color_ok(s->right_bg);
        }
        else if (b->owed[i] == CELLS_OWED_MARKS)
        {
            cells_marks_t *k = &b->what[i].marks;
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
        else if (b->owed[i] == CELLS_OWED_FILL)
        {
            cells_fill_t *e = &b->what[i].fill;
            e->cell.glyph = sst_get_u16(c);
            e->cell.attributes = sst_get_u16(c);
            e->colors.fg_color = sst_get_u16(c);
            e->colors.bg_color = sst_get_u16(c);
            e->colors.ul_color = sst_get_u16(c);
            e->selective = sst_get_bool(c);
            sst_get_u8(c);
            ok &= mode0_glyph_ok(e->cell.glyph, b->width == 40 ? 8 : 16) &&
                  cells_color_ok(e->colors.fg_color) &&
                  cells_color_ok(e->colors.bg_color) && cells_color_ok(e->colors.ul_color);
        }
        else
        {
            ok &= b->owed[i] == CELLS_OWED_NONE;
            for (int k = 0; k < 12; k++)
                sst_get_u8(c);
        }
    }
    b->y_offset = sst_get_u8(c);
    b->hidden = sst_get_u8(c);
    b->all_clean = sst_get_bool(c);
    b->later.n = 0;
    /* Everything here indexes the cell array. row_idx is the scroll remap and
     * y_offset is added to it before the lookup, so both are bounded. */
    if (!ok || !sst_ok(c) || b->y_offset >= TERM_MAX_HEIGHT || b->hidden >= TERM_MAX_HEIGHT)
        return false;
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        if (b->row_idx[i] >= TERM_MAX_HEIGHT)
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
    /* A scroll a screen is owed is done, so the cells saved are the cells. */
    for (int b = 0; b < 2; b++)
    {
        cells_settle(&cells_40.bufs[b]);
        cells_settle(&cells_80.bufs[b]);
    }
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
    sst_put(c, mode0_colors, sizeof mode0_colors);
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
    sst_get(c, mode0_colors, sizeof mode0_colors);
    for (unsigned i = 0; i < MODE0_ENTRIES; i++)
        cells_set_color(i, sst_get_u16(c));
    if (!sst_ok(c))
        return false;
    for (unsigned i = 0; i < MODE0_CELLS; i++)
        if (!cells_color_ok(mode0_colors[i].fg_color) || !cells_color_ok(mode0_colors[i].bg_color) ||
            !cells_color_ok(mode0_colors[i].ul_color))
            return false;
    /* A glyph indexes the font of its size. */
    for (int b = 0; b < (TERM_ALT_SCREEN ? 2 : 1); b++)
    {
        for (unsigned i = 0; i < 40 * TERM_MAX_HEIGHT; i++)
            if (!mode0_glyph_ok(cells_40.bufs[b].mem[i].glyph, 8))
                return false;
        for (unsigned i = 0; i < 80 * TERM_MAX_HEIGHT; i++)
            if (!mode0_glyph_ok(cells_80.bufs[b].mem[i].glyph, 16))
                return false;
    }
    cells_publish();
    return true;
}

void cells_set_height(uint8_t width, uint8_t height)
{
    term_set_height(&cells_term, width == 40 ? &cells_40.grid : &cells_80.grid, height);
    cells_stale = true;
}
