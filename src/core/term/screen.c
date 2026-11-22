/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/screen.h"
#include "core/term/term.h"
#include "machine.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

// The ring a screen's rows are laid out in (screen.h). Every machine keeps
// it for every screen, with cells or without.

// Rows [from, to) keep only the line attributes in keep.
static void screen_lines_keep(screen_t *s, unsigned from, unsigned to, uint8_t keep)
{
    for (unsigned y = from; y < to; y++)
        s->line[screen_mem(s, (uint8_t)y)] &= keep;
}

void screen_lines_normal(screen_t *s, unsigned from, unsigned to)
{
    screen_lines_keep(s, from, to, TERM_LINE_WRAPPED);
}

void screen_lines_blank(screen_t *s, unsigned from, unsigned to)
{
    screen_lines_keep(s, from, to, 0);
}

void screen_lines_unwrap(screen_t *s, unsigned from, unsigned to)
{
    screen_lines_keep(s, from, to, SCREEN_LINE_SIZE);
}

void screen_all_normal(screen_t *s)
{
    for (unsigned m = 0; m < s->mem_rows; m++)
        s->line[m] &= TERM_LINE_WRAPPED;
}

void screen_all_blank(screen_t *s)
{
    memset(s->line, 0, s->mem_rows);
}

void screen_init(screen_t *s, uint8_t rows, uint8_t *line, uint8_t *row_idx)
{
    s->rows = rows;
    s->mem_rows = (uint8_t)(row_idx ? rows + TERM_SPARE_ROWS : rows);
    s->line = line;
    s->row_idx = row_idx;
    s->cells = NULL;
    screen_all_blank(s);
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

// Rows [top, bot] turn n rows up or down round the ring: each takes the
// row of memory n rows below it, or above it, and the n at the end go
// round, their line attributes with them. The rows turn k places toward
// the top: the first k reversed, the rest reversed, then all of them
// reversed. No row needs keeping aside, so a ring of any size turns in
// place.
static void screen_rotate(screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up)
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
        screen_lines_blank(s, edge, (unsigned)bot + 1);
    else
        screen_lines_blank(s, top, (unsigned)edge + 1);
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
    screen_lines_blank(s, in, in + 1u);
    return ghost;
}

bool screen_ring_height(screen_t *s, uint8_t height, bool grow, bool top)
{
    if (!top)
    {
        if (grow)
            screen_lines_blank(s, height - 1u, height);
        return false;
    }
    screen_turn(s, !grow);
    if (!grow)
        s->hidden++;
    else if (s->hidden)
        s->hidden--;
    else
    {
        screen_lines_blank(s, 0, 1);
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
// (grid.c) changes through the screen_ functions. draw.c makes the words
// mode 0 draws the size showing from, and keeps the colors, the blinks and
// the smooth scrolls.
//
// Designed to keep up with 115200 bps without flow control.
// The logic herein will make more sense if you remember this:

// 1. The screen data doesn't move when scrolling. Instead, the rows
//    are remapped through each screen's ring, y_offset and row_idx,
//    and mode 0's row words say where each row's cells are.
// 2. Work on more than one row, a clear, an erase of rows, a rectangle,
//    a scroll between left and right margins, is a job (screen.h), the
//    same work done on each of its rows. A change to one row is done at
//    once.
// 3. What a cell holds is the machine's kind of cell (cell.h). The store
//    moves rows and runs of cells, and reads or writes what is in a cell
//    only through the kind.

bool screen_moved;

/* draw.c's CELL chunk saves each screen's layout (screen_sst_put). The
 * cells are the kind's (cell.h). */
#define SCREEN_SST(X) \
    X(SKIP, screen_moved, "the canvas's load publishes everything")
SST_TABLE(screen_store, SCREEN_SST)

/* The cells' addresses are never written, because the kind's cells are
 * the same in every build of a machine. */
void screen_sst_put(sst_cursor_t *c, screen_t *s)
{
    const uint8_t *const start = c->at;
    screen_job_finish(s);
    for (int i = 0; i < s->rows; i++)
        sst_put_u8(c, s->row_idx[i]);
    sst_put_u8(c, s->spare);
    sst_put(c, s->line, s->mem_rows);
    sst_put_u8(c, s->y_offset);
    sst_put_u8(c, s->hidden);
    assert(!sst_ok(c) || c->at - start == SCREEN_SST_SIZE);
    (void)start;
}

bool screen_sst_get(sst_cursor_t *c, screen_t *s)
{
    screen_cells_t *b = s->cells;
    for (int i = 0; i < s->rows; i++)
        s->row_idx[i] = sst_get_u8(c);
    s->spare = sst_get_u8(c);
    sst_get(c, s->line, s->mem_rows);
    s->y_offset = sst_get_u8(c);
    s->hidden = sst_get_u8(c);
    b->job.kind = SCREEN_JOB_NONE;
    /* Everything here indexes the cell array. row_idx is the ring and
     * y_offset is added to it before the lookup, so both are bounded, and
     * the slots and the spare name every row of memory once. With no spare
     * row, the spare is none. */
    if (!sst_ok(c))
        return false;
    if (s->y_offset >= s->rows || s->hidden >= s->rows)
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

/* ---- cells and rows ---------------------------------------------------- */

// The columns of row y of a screen that has cells: half its width on a
// double-size line.
static inline unsigned screen_cols(const screen_t *s, uint8_t y)
{
    return screen_size(s, y) ? s->cells->width / 2u : s->cells->width;
}

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

// Columns [left, right] of rows [top, bot] become the fill's, with sized
// only those a row's line has: a job, or the row's work at once for one
// row. For a screen with cells.
static void screen_fill_job(screen_t *s, uint8_t top, uint8_t bot, uint8_t left,
                            uint8_t right, bool sized, const cell_fill_t *e)
{
    const screen_job_t j = {.kind = SCREEN_JOB_FILL,
                            .first = top,
                            .last = bot,
                            .left = left,
                            .right = right,
                            .sized = sized,
                            .fill = *e};
    screen_job_start(s, &j);
}

// Rows [from, to) of a screen with cells become the fill's, every column of
// their rows of memory.
static void screen_fill_rows(screen_t *s, uint8_t from, uint8_t to, const cell_fill_t *e)
{
    if (from < to)
        screen_fill_job(s, from, (uint8_t)(to - 1), 0, (uint8_t)(s->cells->width - 1), false,
                        e);
}

// The screen's ring in order, and its height rows erased to the fill.
static void screen_erased(screen_t *s, uint8_t height, const cell_fill_t *e)
{
    screen_ring_clear(s);
    if (s->cells)
        screen_fill_rows(s, 0, height, e);
}

/* ---- what the grid asks of the cells ----------------------------------- */

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
    if (!s->cells)
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

void screen_wipe_rect(screen_t *s, uint8_t top, uint8_t bot, uint8_t left, uint8_t right,
                      bool selective)
{
    const screen_job_t j = {.kind = SCREEN_JOB_WIPE,
                            .first = top,
                            .last = bot,
                            .left = left,
                            .right = right,
                            .sized = true,
                            .wipe = {selective}};
    screen_job_start(s, &j);
}

void screen_fill_rect(term_t *term, screen_t *s, uint8_t top, uint8_t bot, uint8_t left,
                      uint8_t right, uint8_t byte)
{
    if (!s->cells)
        return;
    const cell_fill_t e = screen_glyph_cell(term, s, byte);
    screen_fill_job(s, top, bot, left, right, true, &e);
}

void screen_erase_rect(term_t *term, screen_t *s, uint8_t top, uint8_t bot, uint8_t left,
                       uint8_t right)
{
    if (!s->cells)
        return;
    const cell_fill_t e = screen_pen_erase(term, false, true);
    screen_fill_job(s, top, bot, left, right, true, &e);
}

void screen_erase(term_t *term, screen_t *s, uint8_t y, unsigned from, unsigned to,
                  bool selective, bool written)
{
    if (!s->cells)
        return;
    screen_fill_erased(term, screen_row(s, y), from, to, selective, written);
}

// Columns [x, end) of a row move n right, the fill in coming in at x; or
// n left, the fill in coming in before end.
static void screen_row_insert(cell_t *row, unsigned x, unsigned end, unsigned n,
                              const cell_fill_t *in)
{
    cell_run_move(&row[x + n], &row[x], end - x - n);
    fill_glyph(row, x, x + n, in);
}

static void screen_row_delete(cell_t *row, unsigned x, unsigned end, unsigned n,
                              const cell_fill_t *in)
{
    cell_run_move(&row[x], &row[x + n], end - x - n);
    fill_glyph(row, end - n, end, in);
}

// Erased cells come in: written ones at x with an insert, and ones never
// written before end with a delete.
void screen_insert(term_t *term, screen_t *s, uint8_t y, uint8_t x, unsigned end, unsigned n)
{
    if (!s->cells)
        return;
    const cell_fill_t in = screen_pen_erase(term, false, true);
    screen_row_insert(screen_row(s, y), x, end, n, &in);
}

void screen_delete(term_t *term, screen_t *s, uint8_t y, uint8_t x, unsigned end, unsigned n)
{
    if (!s->cells)
        return;
    const cell_fill_t in = screen_pen_erase(term, false, false);
    screen_row_delete(screen_row(s, y), x, end, n, &in);
}

// The rows go in the order that reads each before it is written: from the
// last up when the rectangle goes down the screen.
void screen_copy_rect(screen_t *s, uint8_t sy, uint8_t sx, uint8_t dy, uint8_t dx, uint8_t h,
                      uint8_t w)
{
    const uint8_t back = dy > sy ? (uint8_t)(h - 1) : 0;
    const screen_job_t j = {.kind = SCREEN_JOB_MOVE,
                            .first = (uint8_t)(dy + back),
                            .last = (uint8_t)(dy + h - 1 - back),
                            .left = dx,
                            .right = (uint8_t)(dx + w - 1),
                            .sized = true,
                            .move = {.row = (uint8_t)(sy + back),
                                     .col = sx,
                                     .moves = h,
                                     .written = true}};
    screen_job_start(s, &j);
}

void screen_attrs(screen_t *s, uint8_t top, uint8_t bot, uint8_t left, uint8_t right,
                  unsigned set, unsigned clear, unsigned toggle)
{
    const screen_job_t j = {.kind = SCREEN_JOB_ATTRS,
                            .first = top,
                            .last = bot,
                            .left = left,
                            .right = right,
                            .sized = true,
                            .attrs = {(uint8_t)set, (uint8_t)clear, (uint8_t)toggle}};
    screen_job_start(s, &j);
}

// SL, SR and the rest, over rows [top, bot]: a job, or the row's work at
// once for one row. A count of a row's columns or more empties the row's
// columns, so the job keeps it to what its uint8_t holds, which is more
// than any row has.
void screen_shift(term_t *term, screen_t *s, uint8_t top, uint8_t bot, unsigned from,
                  unsigned end, unsigned count, bool left)
{
    const screen_job_t j = {
        .kind = SCREEN_JOB_SHIFT,
        .first = top,
        .last = bot,
        .left = (uint8_t)from,
        .right = (uint8_t)(end - 1),
        .sized = true,
        .shift = {count < UINT8_MAX ? (uint8_t)count : UINT8_MAX, left,
                  cell_blank(' ', cell_side_pen(term), false, !left)}};
    screen_job_start(s, &j);
}

void screen_erase_rows(term_t *term, screen_t *s, uint8_t from, uint8_t to, bool selective)
{
    if (!s->cells)
        return;
    const cell_fill_t e = screen_pen_erase(term, selective, false);
    screen_fill_rows(s, from, to, &e);
}

void screen_align(term_t *term, screen_t *s, uint8_t height)
{
    (void)term;
    if (!s->cells)
        return;
    const cell_fill_t e = cell_blank('E', cell_side_default(), false, true);
    screen_fill_rows(s, 0, height, &e);
}

// The rows move round the ring, and the n rows coming in are erased in the
// pen's colors, after the ring has kept the row going out. A screen with no
// cells has only its ring.
uint8_t screen_scroll(term_t *term, screen_t *s, uint8_t top, uint8_t bot, uint8_t n, bool up,
                      bool keep)
{
    if (!s->cells)
        return screen_ring_scroll(s, top, bot, n, up, keep);
    const uint8_t ghost = screen_ring_scroll(s, top, bot, n, up, keep);
    const cell_fill_t e = screen_pen_erase(term, false, false);
    if (up)
        screen_fill_rows(s, (uint8_t)(bot - n + 1), (uint8_t)(bot + 1), &e);
    else
        screen_fill_rows(s, top, (uint8_t)(top + n), &e);
    return ghost;
}

// A row cannot move round the ring, as it does in a scroll of whole rows,
// without taking the cells either side of the columns with it. So the
// cells between the columns move a row at a time, each row read before it
// is written, and the rows keep their line attributes.
void screen_scroll_cols(term_t *term, screen_t *s, uint8_t top, uint8_t bot, unsigned from,
                        unsigned end, uint8_t n, bool up)
{
    const screen_job_t j = {.kind = SCREEN_JOB_MOVE,
                            .first = up ? top : bot,
                            .last = up ? bot : top,
                            .left = (uint8_t)from,
                            .right = (uint8_t)(end - 1),
                            .move = {.row = (uint8_t)(up ? top + n : bot - n),
                                     .col = (uint8_t)from,
                                     .moves = (uint8_t)(bot - top + 1 - n),
                                     .in = screen_pen_erase(term, false, false)}};
    screen_job_start(s, &j);
}

// The fast scroll: the viewport rotates through y_offset, and the row
// coming in is erased in the pen's colors, after the ring has kept the row
// going out.
uint8_t screen_index(term_t *term, screen_t *s, uint8_t height, bool up, bool keep)
{
    if (!s->cells)
        return screen_ring_index(s, height, up, keep);
    const uint8_t ghost = screen_ring_index(s, height, up, keep);
    const uint8_t in = (uint8_t)(up ? height - 1 : 0);
    const cell_fill_t e = screen_pen_erase(term, false, false);
    screen_fill_rows(s, in, (uint8_t)(in + 1), &e);
    return ghost;
}

void screen_clear(term_t *term, screen_t *s, uint8_t height)
{
    const cell_fill_t e = screen_pen_erase(term, false, false);
    screen_erased(s, height, &e);
}

void screen_height(screen_t *s, uint8_t height, bool grow, bool top)
{
    if (!s->cells)
    {
        screen_ring_height(s, height, grow, top);
        return;
    }
    screen_moved = true;
    // With top, a grow moves everything down a row and the row above comes
    // into view: a row of scrollback, which comes back as it left, or else
    // a blank one. A shrink sends the top row out of view to join the
    // scrollback.
    const cell_fill_t e = cell_blank(' ', cell_side_default(), false, false);
    if (screen_ring_height(s, height, grow, top))
        screen_fill_rows(s, 0, 1, &e);
    if (!top && grow)
        // A blank row comes in at the bottom.
        screen_fill_rows(s, (uint8_t)(height - 1), height, &e);
    // Otherwise the bottom row is out of view, and a later grow that brings
    // it back blanks it.
}

// The rows above the view and the rows below it share the ring, so the
// scrollback is at most what the view leaves over.
void screen_fit(screen_t *s, uint8_t height)
{
    screen_ring_fit(s, height);
}

void screen_reset(screen_t *s, uint8_t height)
{
    const cell_fill_t e = cell_blank(' ', cell_side_default(), false, false);
    screen_erased(s, height, &e);
}

bool screen_row_used(screen_t *s, uint8_t y)
{
    if (!s->cells)
        return false;
    const cell_t *row = screen_row(s, y);
    for (unsigned x = 0; x < s->cells->width; x++)
        if (cell_shows(&row[x]))
            return true;
    return false;
}

unsigned screen_row_text(screen_t *s, uint8_t y, uint16_t *out)
{
    if (!s->cells)
        return 0;
    const cell_t *row = screen_row(s, y);
    const unsigned cols = screen_cols(s, y);
    for (unsigned x = 0; x < cols; x++)
        out[x] = cell_text(&row[x]);
    return cols;
}

void screen_blank_cells(cell_t *cells, unsigned n)
{
    const cell_fill_t e = cell_blank(' ', cell_side_default(), false, false);
    cell_run_fill(cells, n, &e);
}

/* ---- jobs -------------------------------------------------------------- */

// The end of the columns the job works on in row y, past its right column,
// or with sized past the row's last column when that comes first.
static unsigned screen_job_end(const screen_t *s, const screen_job_t *j, uint8_t y)
{
    const unsigned end = j->right + 1u;
    if (!j->sized)
        return end;
    const unsigned cols = screen_cols(s, y);
    return end < cols ? end : cols;
}

// A move's row y: the cells of its source row, or the fill coming in.
static void screen_job_move(screen_t *s, const screen_job_t *j, uint8_t y, cell_t *row,
                            unsigned end)
{
    const screen_job_move_t *v = &j->move;
    const unsigned done = y > j->first ? y - j->first : j->first - y;
    if (done >= v->moves)
    {
        fill_glyph(row, j->left, end, &v->in);
        return;
    }
    const uint8_t sy = (uint8_t)(v->row + y - j->first);
    unsigned n = end - j->left;
    if (j->sized)
    {
        const unsigned cols = screen_cols(s, sy);
        if (v->col >= cols)
            return;
        if (n > cols - v->col)
            n = cols - v->col;
    }
    cell_run_move(row + j->left, screen_row(s, sy) + v->col, n);
    if (v->written)
        cell_marks(row, j->left, j->left + n, SCREEN_MARK_WRITTEN, 0, 0);
}

// The job's work on row y.
static void screen_job_row(screen_t *s, const screen_job_t *j, uint8_t y)
{
    const unsigned left = j->left, end = screen_job_end(s, j, y);
    if (left >= end)
        return;
    cell_t *row = screen_row(s, y);
    switch (j->kind)
    {
    case SCREEN_JOB_FILL:
        fill_glyph(row, left, end, &j->fill);
        break;
    case SCREEN_JOB_MOVE:
        screen_job_move(s, j, y, row, end);
        break;
    case SCREEN_JOB_SHIFT:
    {
        const unsigned n = j->shift.n < end - left ? j->shift.n : end - left;
        if (j->shift.left)
            screen_row_delete(row, left, end, n, &j->shift.in);
        else
            screen_row_insert(row, left, end, n, &j->shift.in);
        break;
    }
    case SCREEN_JOB_ATTRS:
        cell_marks(row, left, end, j->attrs.set, j->attrs.clear, j->attrs.toggle);
        break;
    case SCREEN_JOB_WIPE:
        cell_wipe(row, left, end, j->wipe.selective);
        break;
    }
}

void screen_job_start(screen_t *s, const screen_job_t *job)
{
    if (!s->cells)
        return;
    assert(!screen_job_pending(s));
    if (job->first == job->last)
    {
        screen_job_row(s, job, job->first);
        return;
    }
    screen_job_t *j = &s->cells->job;
    *j = *job;
    j->next = j->first;
    screen_job_finish(s);
}

bool screen_job_step(screen_t *s, unsigned rows)
{
    screen_job_t *j = &s->cells->job;
    for (; rows && j->kind != SCREEN_JOB_NONE; rows--)
    {
        const uint8_t y = j->next;
        screen_job_row(s, j, y);
        if (y == j->last)
            j->kind = SCREEN_JOB_NONE;
        else
            j->next = (uint8_t)(y < j->last ? y + 1 : y - 1);
    }
    return j->kind != SCREEN_JOB_NONE;
}

void screen_job_finish(screen_t *s)
{
    if (s->cells)
        screen_job_step(s, UINT_MAX);
}
