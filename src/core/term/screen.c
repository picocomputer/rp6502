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
// 3. What a cell holds is the machine's kind of cell (cell.h). The store
//    moves rows and runs of cells, and keeps what is owed, and reads or
//    writes what is in a cell only through the kind.

bool screen_moved;

/* draw.c's CELL chunk saves each screen's layout (screen_sst_put). The
 * cells are the kind's (cell.h). */
#define SCREEN_SST(X) \
    X(SKIP, screen_moved, "the canvas's load publishes everything")
SST_TABLE(screen_store, SCREEN_SST)

/* A record is padded to SCREEN_SST_RECORD bytes with zeros. */
_Static_assert(CELL_FILL_SST_SIZE <= SCREEN_SST_RECORD &&
                   4 + 2 * CELL_SIDE_SST_SIZE <= SCREEN_SST_RECORD && 5 <= SCREEN_SST_RECORD,
               "a record the size of what it is owed");

static void screen_sst_zeros(sst_cursor_t *c, unsigned n)
{
    for (; n; n--)
        sst_put_u8(c, 0);
}

static void screen_sst_skip(sst_cursor_t *c, unsigned n)
{
    for (; n; n--)
        sst_get_u8(c);
}

/* The cells' addresses are never written, because the kind's cells are
 * the same in every build of a machine. */
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
            cell_side_put(c, &h->left);
            cell_side_put(c, &h->right);
            screen_sst_zeros(c, SCREEN_SST_RECORD - 4 - 2 * CELL_SIDE_SST_SIZE);
        }
        else if (b->owed[i] == SCREEN_OWED_MARKS)
        {
            const screen_marks_t *k = &b->what[i].marks;
            sst_put_u8(c, k->from);
            sst_put_u8(c, k->to);
            sst_put_u8(c, k->set);
            sst_put_u8(c, k->clear);
            sst_put_u8(c, k->toggle);
            screen_sst_zeros(c, SCREEN_SST_RECORD - 5);
        }
        else if (b->owed[i] == SCREEN_OWED_FILL)
        {
            cell_fill_put(c, &b->what[i].fill);
            screen_sst_zeros(c, SCREEN_SST_RECORD - CELL_FILL_SST_SIZE);
        }
        else
            screen_sst_zeros(c, SCREEN_SST_RECORD);
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
            ok &= cell_side_get(c, &h->left);
            ok &= cell_side_get(c, &h->right);
            screen_sst_skip(c, SCREEN_SST_RECORD - 4 - 2 * CELL_SIDE_SST_SIZE);
            /* The record indexes the row's cells. */
            ok &= h->cols <= b->width && h->end <= h->cols && h->to <= h->end &&
                  h->from + h->end - h->to <= h->cols;
        }
        else if (b->owed[i] == SCREEN_OWED_MARKS)
        {
            screen_marks_t *k = &b->what[i].marks;
            k->from = sst_get_u8(c);
            k->to = sst_get_u8(c);
            k->set = sst_get_u8(c);
            k->clear = sst_get_u8(c);
            k->toggle = sst_get_u8(c);
            screen_sst_skip(c, SCREEN_SST_RECORD - 5);
            /* The record indexes the row's cells. */
            ok &= k->from <= k->to && k->to <= b->width;
        }
        else if (b->owed[i] == SCREEN_OWED_FILL)
        {
            ok &= cell_fill_get(c, &b->what[i].fill, b->width);
            screen_sst_skip(c, SCREEN_SST_RECORD - CELL_FILL_SST_SIZE);
        }
        else
        {
            ok &= b->owed[i] == SCREEN_OWED_NONE;
            screen_sst_skip(c, SCREEN_SST_RECORD);
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

/* ---- cells and rows ---------------------------------------------------- */

// Fill [start, end) of `row`: every cell becomes the fill's cell. A
// selective erase leaves the guarded cells as they are.
static void fill_glyph(cell_t *row, unsigned start, unsigned end, const cell_fill_t *e)
{
    if (!e->selective)
    {
        cell_run_fill(row + start, end - start, e);
        return;
    }
    cell_run_fill_sel(row, start, end, e);
}

// An erase in the pen's colors, selective or not, written or not.
static cell_fill_t screen_pen_erase(const term_t *term, bool selective, bool written)
{
    return cell_blank(' ', cell_side_pen(term), selective, written);
}

// Erase [start, end) of `row` in the pen's colors.
static void screen_fill_erased(term_t *term, cell_t *row, unsigned start, unsigned end,
                               bool selective, bool written)
{
    const cell_fill_t e = screen_pen_erase(term, selective, written);
    fill_glyph(row, start, end, &e);
}

// Pay row m of memory what it is owed. A shift moves the survivors, then
// blanks the columns either side of where they landed: those on the left
// came in with a shift right, so are written, and those on the right are
// not (screen_shift).
static void screen_pay(screen_cells_t *buf, uint8_t m)
{
    cell_t *row = buf->mem + (uint32_t)m * buf->width;
    const uint8_t owed = buf->owed[m];
    buf->owed[m] = SCREEN_OWED_NONE;
    if (owed == SCREEN_OWED_FILL)
        fill_glyph(row, 0, buf->width, &buf->what[m].fill);
    else if (owed == SCREEN_OWED_MARKS)
    {
        const screen_marks_t *k = &buf->what[m].marks;
        cell_marks(row, k->from, k->to, k->set, k->clear, k->toggle);
    }
    else if (owed == SCREEN_OWED_SHIFT)
    {
        const screen_shift_t *s = &buf->what[m].shift;
        cell_run_move(row + s->to, row + s->from, (unsigned)(s->end - s->to));
        if (s->to)
        {
            const cell_fill_t e = cell_blank(' ', s->left, false, true);
            cell_run_fill(row, s->to, &e);
        }
        if (s->end < s->cols)
        {
            const cell_fill_t e = cell_blank(' ', s->right, false, false);
            cell_run_fill(row + s->end, (unsigned)(s->cols - s->end), &e);
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
                                  const cell_fill_t *e)
{
    screen_cells_t *b = s->cells;
    for (uint8_t i = from; i < to; i++)
    {
        const uint8_t m = screen_mem(s, i);
        cell_fill_t *fill = &b->what[m].fill;
        if (e->selective && b->owed[m] >= SCREEN_OWED_SHIFT)
            screen_pay(b, m);
        else if (e->selective && b->owed[m] == SCREEN_OWED_FILL && !fill->selective)
        {
            if (cell_guarded(&fill->cell))
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
    const cell_fill_t e = cell_blank(' ', cell_side_default(), false, false);
    screen_mark_rows_with(s, from, to, &e);
}

// Lazy-erase rows [from, to) of a screen in the pen's colors.
static void screen_mark_rows_erase(term_t *term, screen_t *s, uint8_t from, uint8_t to,
                                   bool selective, bool written)
{
    const cell_fill_t e = screen_pen_erase(term, selective, written);
    screen_mark_rows_with(s, from, to, &e);
}

// Lazy clear of the given screen: its ring in order, and every visible row
// marked dirty with the given erase. A scroll the screen was owed is
// forgotten with the cells it would have moved. No cell memory is touched
// here -- draw.c's task drains dirty rows in the background (one row per
// tick, on both screens), and the active screen's cursor row is also
// cleaned synchronously when the terminal touches it.
static void screen_mark_erased(screen_t *s, uint8_t height, const cell_fill_t *e)
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

// The cell a glyph lands as on a screen, in the pen, through the active
// character set: DEC Special Graphics for that set's 0x5F to 0x7E, DEC
// Technical for that set's 0x21 to 0x7E, the italics for SGR 3's ASCII,
// the code page's otherwise. The UK set's pound sign is the DEC set's 0x7D.
static cell_fill_t screen_glyph_cell(term_t *term, const screen_t *s, uint8_t byte)
{
    const term_pen_t *pen = term_pen(term);
    const uint8_t cs = pen->g[pen->ss ? pen->ss : pen->gl];
    uint8_t code = byte;
    unsigned set = CELL_SET_PAGE;
    if (cs == TERM_CS_DEC && byte >= 0x5F && byte <= 0x7E)
        set = CELL_SET_DEC;
    else if (cs == TERM_CS_TECH && byte >= 0x21 && byte <= 0x7E)
        set = CELL_SET_TECH;
    else if (cs == TERM_CS_UK && byte == '#')
    {
        code = 0x7D;
        set = CELL_SET_DEC;
    }
    else if ((pen->sgr_attr & TERM_ATTR_ITALIC) && byte < 0x80)
        set = CELL_SET_ITALIC;
    return cell_from_pen(term, s->cells->width, code, set);
}

void screen_glyphs(term_t *term, screen_t *s, uint8_t y, uint8_t x, uint8_t byte, unsigned n)
{
    if (!screen_settled(s))
        return;
    const cell_fill_t e = screen_glyph_cell(term, s, byte);
    cell_t *at = screen_row(s, y) + x;
    if (n == 1)
    {
        cell_put1(at, &e);
        return;
    }
    cell_run_fill(at, n, &e);
}

void screen_wipe(term_t *term, screen_t *s, uint8_t y, unsigned from, unsigned to,
                 bool selective)
{
    (void)term;
    if (!screen_settled(s))
        return;
    cell_wipe(screen_row(s, y), from, to, selective);
}

void screen_fill_row(term_t *term, screen_t *s, uint8_t y, uint8_t byte)
{
    if (!screen_settled(s))
        return;
    const cell_fill_t e = screen_glyph_cell(term, s, byte);
    screen_mark_rows_with(s, y, (uint8_t)(y + 1), &e);
}

void screen_erase(term_t *term, screen_t *s, uint8_t y, unsigned from, unsigned to,
                  bool selective, bool written)
{
    if (!screen_settled(s))
        return;
    screen_fill_erased(term, screen_row(s, y), from, to, selective, written);
}

// Columns [x, end) of a row move n right, written erased cells coming in
// at x; or n left, erased cells never written coming in before end.
static void screen_row_insert(term_t *term, cell_t *row, unsigned x, unsigned end,
                              unsigned n)
{
    cell_run_move(&row[x + n], &row[x], end - x - n);
    screen_fill_erased(term, row, x, x + n, false, true);
}

static void screen_row_delete(term_t *term, cell_t *row, unsigned x, unsigned end,
                              unsigned n)
{
    cell_run_move(&row[x], &row[x + n], end - x - n);
    screen_fill_erased(term, row, end - n, end, false, false);
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
    cell_t *row = screen_row(s, dy);
    cell_run_move(row + dx, screen_row(s, sy) + sx, n);
    cell_marks(row, dx, dx + n, SCREEN_MARK_WRITTEN, 0, 0);
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

// One more shift of n columns, with this blank, added to the shift a row of
// cols columns is owed.
static inline void screen_shift_add(screen_shift_t *s, uint8_t cols, uint8_t n, bool left,
                                    cell_side_t side)
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
        s->right = side;
    }
    else
    {
        s->to = (uint8_t)(s->to + n < cols ? s->to + n : cols);
        s->end = (uint8_t)(s->end + n < cols ? s->end + n : cols);
        s->left = side;
    }
    if (s->to == s->end)
        s->from = 0;
}

// SL and SR only mark the rows: each is owed the shift, and no cell moves
// until the row is paid. A row that already owes a shift has this one
// added to its record. It is paid first only when this shift's blanks
// would sit beside blanks unlike them on the same side. A row owed a
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
    const cell_side_t side = cell_side_pen(term);
    const cell_fill_t in = cell_blank(' ', side, false, !left);
    const uint8_t width = buf->width;
    screen_shift_t whole = {0, 0, width, width, side, side};
    screen_shift_add(&whole, width, count < width ? (uint8_t)count : width, left, side);
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
        screen_shift_t r = {0, 0, cols, cols, side, side};
        if (owed == SCREEN_OWED_FILL)
        {
            const cell_fill_t *fill = &buf->what[m].fill;
            if (cell_fill_same(fill, &in))
                continue;
            screen_pay(buf, m);
        }
        else if (owed == SCREEN_OWED_SHIFT)
        {
            // A shift owed from when the row had another line size pays
            // first: its columns are not this shift's.
            if (s->cols != cols ||
                (n < cols && (left ? s->end < cols && !cell_side_same(&s->right, &side)
                                   : s->to && !cell_side_same(&s->left, &side))))
                screen_pay(buf, m);
            else
                r = *s;
        }
        else if (owed == SCREEN_OWED_MARKS)
            screen_pay(buf, m);
        screen_shift_add(&r, cols, n, left, side);
        buf->owed[m] = SCREEN_OWED_SHIFT;
        *s = r;
    }
    buf->all_clean = false;
    screen_clean_line(sc, sc->pos.y);
}

void screen_erase_rows(term_t *term, screen_t *s, uint8_t from, uint8_t to, bool selective,
                       bool written)
{
    if (!screen_settled(s))
        return;
    screen_mark_rows_erase(term, s, from, to, selective, written);
}

void screen_align(term_t *term, screen_t *s, uint8_t height)
{
    (void)term;
    if (!screen_settled(s))
        return;
    const cell_fill_t e = cell_blank('E', cell_side_default(), false, true);
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
    const uint8_t ghost = screen_ring_scroll(s, top, bot, n, up, keep);
    if (up)
        screen_mark_rows_erase(term, s, (uint8_t)(bot - n + 1), (uint8_t)(bot + 1), false,
                               false);
    else
        screen_mark_rows_erase(term, s, top, (uint8_t)(top + n), false, false);
    return ghost;
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
    if (n < h && (outside > CELL_STRIP_MAX || inside * (h - n) <= outside * h))
    {
        for (unsigned i = 0; i < h - n; i++)
        {
            const unsigned to = l.up ? top + i : bot - i;
            cell_run_move(screen_row(s, (uint8_t)to) + from,
                          screen_row(s, (uint8_t)(l.up ? to + n : to - n)) + from, inside);
        }
    }
    else if (n < h)
    {
        screen_rotate(s, (uint8_t)top, (uint8_t)bot, (uint8_t)n, l.up);
        // Row j of the region now has the cells either side, and the line
        // attributes, that belong on row j + step, counted round the region.
        const unsigned step = l.up ? n : h - n;
        cell_strip_t strip;
        for (unsigned first = 0, moved = 0; moved < h; first++)
        {
            cell_t *row = screen_row(s, (uint8_t)(top + first));
            uint8_t *line = &s->line[screen_mem(s, (uint8_t)(top + first))];
            const uint8_t kept = *line;
            cell_strip_keep(&strip, row, from, end, width);
            for (unsigned j = first;;)
            {
                unsigned src = j >= step ? j - step : j + h - step;
                moved++;
                if (src == first)
                {
                    cell_strip_put(&strip, row, from, end, width);
                    *line = kept;
                    break;
                }
                const uint8_t m = screen_mem(s, (uint8_t)(top + src));
                cell_t *other = screen_row(s, (uint8_t)(top + src));
                cell_strip_move(row, other, from, end, width);
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
    const cell_fill_t in = screen_pen_erase(term, false, false);
    const unsigned h = (unsigned)bot - top + 1;
    if (l->n && l->top == top && l->bot == bot && l->from == from && l->end == end &&
        l->up == up && cell_fill_same(&l->in, &in))
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
    const uint8_t ghost = screen_ring_index(s, height, up, keep);
    if (up)
        screen_mark_rows_erase(term, s, (uint8_t)(height - 1), height, false, false);
    else
        screen_mark_rows_erase(term, s, 0, 1, false, false);
    return ghost;
}

void screen_clear(term_t *term, screen_t *s, uint8_t height)
{
    if (!s->cells)
    {
        screen_ring_clear(s);
        return;
    }
    const cell_fill_t e = screen_pen_erase(term, false, false);
    screen_mark_erased(s, height, &e);
}

void screen_height(screen_t *s, uint8_t height, bool grow, bool top)
{
    if (!screen_settled(s))
    {
        screen_ring_height(s, height, grow, top);
        return;
    }
    screen_moved = true;
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
    const cell_fill_t e = cell_blank(' ', cell_side_default(), false, false);
    screen_mark_erased(s, height, &e);
}

bool screen_row_used(screen_t *s, uint8_t y)
{
    if (!screen_settled(s))
        return false;
    screen_clean_line(s, y);
    const cell_t *row = screen_row(s, y);
    for (unsigned x = 0; x < s->cells->width; x++)
        if (cell_shows(&row[x]))
            return true;
    return false;
}

unsigned screen_row_text(screen_t *s, uint8_t y, uint16_t *out)
{
    if (!screen_settled(s))
        return 0;
    screen_clean_line(s, y);
    const cell_t *row = screen_row(s, y);
    const unsigned cols = screen_size(s, y) ? s->cells->width / 2u : s->cells->width;
    for (unsigned x = 0; x < cols; x++)
        out[x] = cell_text(&row[x]);
    return cols;
}

void screen_blank_cells(cell_t *cells, unsigned n)
{
    const cell_fill_t e = cell_blank(' ', cell_side_default(), false, false);
    cell_run_fill(cells, n, &e);
}
