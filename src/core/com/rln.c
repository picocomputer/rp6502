/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/timer.h"
#include "machine.h"
#include "core/api/api.h"
#include "core/com/console.h"
#include "core/com/rln.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/com/tio.h"
#include "core/sys/driver.h"
#include "core/term/cells.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

/* Console manifold compatibility rules.
**
** rln drives a "manifold" of terminals: the UART, telnet and the screen. It
** lays the line out from the console's terminal, which has seen everything
** sent to all of them, so the line looks right on every terminal at least as
** wide as the screen. Callers can place rln inside a wider form by setting
** rln_max_length to reserve only the cells rln is allowed to own.
**
** Runtime invariants:
**   - rln breaks its own rows: CR LF right after the last column. It never
**     relies on autowrap, and no terminal is trusted to reflow: a new width
**     starts the line over on a clean row.
**   - Moves are relative: CUU and CUD between rows, CR and CUF along one.
**     No absolute Y; no writes past rln_max_length.
**   - Inside left and right margins a row is the columns between them: a
**     CR lands on the left margin, and the row breaks at the right one.
**   - rln owns only rln_max_length cells from the start of input, so no EL
**     unless the input can take more than its row.
**   - A plain line (rln_set_plain, or a script read) lays nothing out and
**     sends no escape sequence: it echoes, moves left with BS and right by
**     printing what is there, erases with spaces, and the terminal wraps it.
**   - A terminal in VT52 mode is sent a VT52's commands: ESC A, B and C to
**     move, each once for each row or column, and ESC K to erase. It has no
**     insert mode and is sent no cursor shape.
*/

#define RLN_BUF_SIZE 256
_Static_assert(RLN_BUF_SIZE == RLN_LINE_MAX, "rln.h publishes this bound");
#define RLN_LASTKEY_MAX 32
_Static_assert(RLN_LASTKEY_MAX >= TIO_ITEM_MAX, "every key fits the last key");

// rln_caps modes — public ABI is uint8_t, so the values are wire values.
#define RLN_CAPS_OFF 0
#define RLN_CAPS_UPPER 1
#define RLN_CAPS_SWAP 2
// Terms < 29 cols may not reach 254/255 chars in 10 rows.
#define RLN_MAX_ROWS 10

// Insert mode goes off at every read that lays its line out, and stays off,
// because rln overwrites the line as it draws it again.
#define RLN_MODES "\33[4l"

// The 6502's poke stream is a source of its own, after the com sources.
#define RLN_POKE COM_SOURCE_ANY
#define RLN_SOURCE_COUNT (RLN_POKE + 1)

// The history of the read in progress, its caller's, or NULL for none.
static rln_history_t *rln_history;

// Input state
static char rln_buf[RLN_BUF_SIZE];

const char *rln_line(void)
{
    return rln_buf;
}
static rln_read_callback_t rln_callback;
static com_source_t rln_line_src;
static timer_mach_t rln_idle_deadline;
static uint8_t rln_buflen;
static uint8_t rln_bufpos;
static uint8_t rln_max_length;
static uint32_t rln_idle_timeout_ms;
static uint8_t rln_caps;
// 6502 attribute: when set, the line-terminating newline is suppressed
// so field input on the last line keeps the cursor on its row. Persists
// across reads; reset on stop() (via rln_init).
static bool rln_suppress_newline;

// Display state. The line is laid out at the prompt column and the width
// the console's terminal had when the read started or last changed size.
static bool rln_plain;                 // lay no line out: rln_set_plain
static uint16_t rln_prompt_col;        // 1-based from where a CR lands, 0 = no prompt column, no wrapping
static uint16_t rln_width;             // the columns of the prompt's row, between the margins it is in
static uint8_t rln_grid_width;         // the shown grid's width, watched for a change
static uint8_t rln_rows;               // the most rows the line may take
static uint8_t rln_cur_idx;            // buffer index whose screen position the cursor is at
static bool rln_overwrite;             // preserved across rln_read_line calls; cleared by rln_init (rln_stop)
static uint8_t rln_rendered_max_row;   // highest row index rln has written to in the current line
static uint8_t rln_last_render_buflen; // buflen as of the last render
// The terminal's erase reaches the end of the line's row and no further: its
// right margin is the row's last column, as it was when the line was laid
// out. Otherwise the line erases with spaces.
static bool rln_erase_el;

// A program's own size for the console's terminal, while either is pinned:
// a third grid, which the line editor and the terminal's answers use.
static uint16_t rln_pinned_width;
static uint16_t rln_pinned_height;
static term_grid_t rln_grid;
static uint8_t rln_grid_lines[2 * RLN_GRID_ROWS];

// The CR or LF each source, the poke stream included, last ended a line
// with, so the other half of a CRLF pair arriving next is absorbed instead of
// ending a second line.
static uint8_t rln_line_ends[RLN_SOURCE_COUNT];

// Lastkey capture for the 6502 API. rln_action_taken is the live
// "this dispatch mutated state" flag, sampled at lastkey publish time.
// Typed and poked dispatches both populate these; an item rln_item drops
// never reaches them.
static uint8_t rln_lastkey_buf[RLN_LASTKEY_MAX];
static uint8_t rln_lastkey_len;
static bool rln_action_taken;
static bool rln_lastkey_action;
/* A read waits to be laid out until what was written before it has reached
 * the terminal, a line still held for a smooth scroll included, because the
 * line starts where the cursor is then. Its keys wait with it. */
static bool rln_laying_out;

static bool rln_has_grid(void)
{
    return cells_term.grid_count > TERM_SCREEN_GRIDS;
}

/* ----- The reply -----
 *
 * What a key makes the line editor send is its reply. It is recorded here
 * and sent as the console's line takes it, and no key is taken until the
 * last key's reply has gone out, as on a Pico, whose UART holds the editor
 * up a byte at a time. A reply is pieces: bytes of its own, a run of the
 * line, and a byte or an escape pair over and over, so a whole line drawn
 * again is a few pieces rather than a copy of the line. A reply that ends a
 * read hands the line over only once it has gone out, so what the program
 * writes next comes after it. A reply too big to record goes out at once, as
 * one sent at a stop does. */
#define RLN_OUT_LIT 192
#define RLN_OUT_PIECES 40
enum
{
    RLN_OUT_BYTES,  /* rln_out_lit[a] on, n of them */
    RLN_OUT_SCREEN, /* the same, for the screen alone */
    RLN_OUT_RUN,    /* rln_buf[a] on, n of them */
    RLN_OUT_REP,    /* byte a, n times */
    RLN_OUT_ESC,    /* ESC and byte a, n times */
    RLN_OUT_KINDS
};
typedef struct
{
    uint8_t kind;
    uint8_t a;
    uint8_t n;
} rln_piece_t;
_Static_assert(RLN_OUT_LIT <= UINT8_MAX + 1, "a piece's byte offset is a uint8_t");
static rln_piece_t rln_out[RLN_OUT_PIECES];
static char rln_out_lit[RLN_OUT_LIT];
static uint8_t rln_out_len;     /* pieces recorded */
static uint8_t rln_out_lit_len; /* bytes of their own recorded */
static uint8_t rln_out_at;      /* the piece going out */
static uint16_t rln_out_sent;   /* its bytes gone out */
/* The read the reply ends, handed its line once the reply is out. */
static rln_read_callback_t rln_out_then;
static bool rln_out_timed_out;

static uint16_t HOST_IN_RAM_FUNC rln_out_size(const rln_piece_t *p)
{
    return p->kind == RLN_OUT_ESC ? (uint16_t)(2u * p->n) : p->n;
}

static char HOST_IN_RAM_FUNC rln_out_byte(const rln_piece_t *p, uint16_t i)
{
    switch (p->kind)
    {
    case RLN_OUT_RUN:
        return rln_buf[p->a + i];
    case RLN_OUT_REP:
        return (char)p->a;
    case RLN_OUT_ESC:
        return i & 1 ? (char)p->a : '\33';
    default:
        return rln_out_lit[p->a + i];
    }
}

/* Sends what the console's line takes, or all of it with force, and true
 * once the reply has gone out and its read has its line. */
static bool HOST_IN_RAM_FUNC rln_out_send(bool force)
{
    for (; rln_out_at < rln_out_len; rln_out_at++, rln_out_sent = 0)
    {
        const rln_piece_t *p = &rln_out[rln_out_at];
        const uint16_t size = rln_out_size(p);
        if (p->kind == RLN_OUT_SCREEN)
        {
            if (!force && manifold_tx_room() < size)
                return false;
            manifold_write_to(COM_TERM_SCREEN, &rln_out_lit[p->a], size);
            continue;
        }
        for (; rln_out_sent < size; rln_out_sent++)
        {
            if (!force && !manifold_cts())
                return false;
            manifold_putchar(rln_out_byte(p, rln_out_sent));
        }
    }
    rln_out_len = rln_out_lit_len = rln_out_at = 0;
    rln_out_sent = 0;
    rln_read_callback_t then = rln_out_then;
    if (then)
    {
        rln_out_then = NULL;
        then(rln_out_timed_out, rln_buf);
    }
    return true;
}

/* The next piece, with room for its bytes of its own, the reply so far
 * going out at once to make room. */
static rln_piece_t *rln_out_new(uint8_t kind, uint8_t a, uint8_t n)
{
    const bool own = kind == RLN_OUT_BYTES || kind == RLN_OUT_SCREEN;
    if (rln_out_len == RLN_OUT_PIECES ||
        (own && rln_out_lit_len + n > RLN_OUT_LIT))
        rln_out_send(true);
    rln_piece_t *p = &rln_out[rln_out_len++];
    *p = (rln_piece_t){kind, own ? rln_out_lit_len : a, own ? 0 : n};
    return p;
}

static rln_piece_t *rln_out_last(uint8_t kind)
{
    if (!rln_out_len || rln_out_len == rln_out_at)
        return NULL;
    rln_piece_t *p = &rln_out[rln_out_len - 1];
    return p->kind == kind ? p : NULL;
}

static void rln_out_c(char c)
{
    rln_piece_t *p = rln_out_last(RLN_OUT_BYTES);
    if (!p || p->n == UINT8_MAX || rln_out_lit_len == RLN_OUT_LIT)
        p = rln_out_new(RLN_OUT_BYTES, 0, 1);
    rln_out_lit[rln_out_lit_len++] = c;
    p->n++;
}

static void rln_out_s(const char *str)
{
    while (*str)
        rln_out_c(*str++);
}

static void rln_out_buf(uint8_t i)
{
    rln_piece_t *p = rln_out_last(RLN_OUT_RUN);
    if (p && p->a + p->n == i && p->n < UINT8_MAX)
        p->n++;
    else
        rln_out_new(RLN_OUT_RUN, i, 1);
}

static void rln_out_rep(uint8_t kind, char c, unsigned n)
{
    for (; n; n--)
    {
        rln_piece_t *p = rln_out_last(kind);
        if (p && p->a == (uint8_t)c && p->n < UINT8_MAX)
            p->n++;
        else
            rln_out_new(kind, (uint8_t)c, 1);
    }
}

static void rln_out_screen(const char *seq, uint8_t n)
{
    rln_piece_t *p = rln_out_new(RLN_OUT_SCREEN, 0, n);
    memcpy(&rln_out_lit[rln_out_lit_len], seq, n);
    rln_out_lit_len += n;
    p->n = n;
}

/* A reply is waiting to go out, or its read to have its line. */
bool HOST_IN_RAM_FUNC rln_replying(void)
{
    return rln_out_len || rln_out_then;
}

/* A callback is a function pointer, which cannot be written to a blob, so
 * it is saved as a token. console.c's reader is the only one this file can
 * name, and any other callback fails the save rather than being restored
 * as the wrong reader. Its reads have no history, so none is carried. */
static void rln_sst_put_one(sst_cursor_t *c, rln_read_callback_t reader)
{
    if (!reader)
        sst_put_u8(c, 0);
    else if (reader == console_rln_reader())
        sst_put_u8(c, 1);
    else
        sst_fail(c);
}

static bool rln_sst_get_one(sst_cursor_t *c, rln_read_callback_t *reader)
{
    uint8_t token = sst_get_u8(c);
    if (token > 1)
        return sst_refuse(c, "a reader rln cannot name");
    *reader = token ? console_rln_reader() : NULL;
    return true;
}

/* The read in progress, and the one a reply still going out ends. */
static void rln_sst_put_reader(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    rln_sst_put_one(c, rln_callback);
    rln_sst_put_one(c, rln_out_then);
}

static bool rln_sst_get_reader(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    rln_history = NULL;
    return rln_sst_get_one(c, &rln_callback) &&
           rln_sst_get_one(c, &rln_out_then);
}

/* Whether the third grid is on the console's terminal, and the grid. */
static void rln_sst_put_grid(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_bool(c, rln_has_grid());
    term_grid_sst_put(c, &rln_grid);
}

static bool rln_sst_get_grid(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    bool grid = sst_get_bool(c);
    if (!sst_ok(c))
        return false;
    if (grid && !rln_has_grid())
        term_add_grid(&cells_term, &rln_grid, cells_shown(&cells_term),
                      rln_grid_lines, RLN_GRID_ROWS);
    else if (!grid && rln_has_grid())
        term_drop_grid(&cells_term, &rln_grid);
    return term_grid_sst_get(c, &rln_grid);
}

/* The lengths need no range check because a uint8_t cannot reach past a
 * buffer this size, and the static assert keeps that true if RLN_BUF_SIZE
 * changes. The indexes are checked because they can exceed the lengths they
 * index. A line with a prompt column wraps at its width, so that width is
 * never 0. */
_Static_assert(RLN_BUF_SIZE > UINT8_MAX,
               "a byte length must not escape rln_buf");
static bool rln_sst_valid(sst_cursor_t *c)
{
    if (rln_bufpos > rln_buflen || rln_cur_idx > rln_buflen ||
        rln_lastkey_len > RLN_LASTKEY_MAX)
        return sst_refuse(c, "an index past its line");
    if (rln_prompt_col && !rln_width)
        return sst_refuse(c, "a prompt column with no width");
    if (rln_out_len > RLN_OUT_PIECES || rln_out_at > rln_out_len ||
        rln_out_lit_len > RLN_OUT_LIT)
        return sst_refuse(c, "a reply past its pieces");
    for (unsigned i = 0; i < rln_out_len; i++)
    {
        const rln_piece_t *p = &rln_out[i];
        const bool own = p->kind == RLN_OUT_BYTES || p->kind == RLN_OUT_SCREEN;
        if (p->kind >= RLN_OUT_KINDS ||
            (own && p->a + p->n > rln_out_lit_len) ||
            (p->kind == RLN_OUT_RUN && p->a + p->n > RLN_BUF_SIZE) ||
            (p->kind == RLN_OUT_SCREEN && p->n > MANIFOLD_TX_SIZE))
            return sst_refuse(c, "a piece of a reply past its bytes");
    }
    if (rln_out_at < rln_out_len && rln_out_sent > rln_out_size(&rln_out[rln_out_at]))
        return sst_refuse(c, "a piece sent past its end");
    return true;
}

#define RLN_PIECE_ROWS(Y, p) Y(U8, p.kind) Y(U8, p.a) Y(U8, p.n)
#define RLN_SST_ROWS(X)                                                 \
    X(BYTES, rln_buf)                                                   \
    X(FN, rln_sst_put_reader, rln_sst_get_reader, 2, 1)                 \
    X(IN_FN, rln_out_then)                                              \
    X(STRUCTS, rln_out, RLN_PIECE_ROWS)                                 \
    X(BYTES, rln_out_lit)                                               \
    X(U8, rln_out_len)                                                  \
    X(U8, rln_out_lit_len)                                              \
    X(U8, rln_out_at)                                                   \
    X(U16, rln_out_sent)                                                \
    X(BOOL, rln_out_timed_out)                                          \
    X(BOOL, rln_erase_el)                                               \
    X(IN_FN, rln_callback)                                              \
    X(IN_FN, rln_history, "its caller's, so the load sets none")        \
    X(U64, rln_idle_deadline, "machine time, so carried whole")         \
    X(U8, rln_buflen)                                                   \
    X(U8, rln_bufpos)                                                   \
    X(U8, rln_max_length)                                               \
    X(U32, rln_idle_timeout_ms)                                         \
    X(U8, rln_caps)                                                     \
    X(BOOL, rln_suppress_newline)                                       \
    X(U16, rln_prompt_col)                                              \
    X(U16, rln_width)                                                   \
    X(U8, rln_grid_width)                                               \
    X(U8, rln_rows)                                                     \
    X(U8, rln_cur_idx)                                                  \
    X(BOOL, rln_overwrite)                                              \
    X(U8, rln_rendered_max_row)                                         \
    X(U8, rln_last_render_buflen)                                       \
    X(BYTES, rln_lastkey_buf)                                           \
    X(U8, rln_lastkey_len)                                              \
    X(BOOL, rln_action_taken)                                           \
    X(BOOL, rln_lastkey_action)                                         \
    X(BOOL, rln_laying_out)                                             \
    X(BYTES, rln_line_ends)                                             \
    X(U16, rln_pinned_width)                                            \
    X(U16, rln_pinned_height)                                           \
    X(VALIDATE, rln_sst_valid)                                          \
    X(FN, rln_sst_put_grid, rln_sst_get_grid,                           \
      1 + TERM_GRID_SST_SIZE(RLN_GRID_ROWS), 1)                         \
    X(IN_FN, rln_grid)                                                  \
    X(IN_FN, rln_grid_lines)                                            \
    X(SKIP, rln_line_src, "read only inside the read's callback")       \
    X(SKIP, rln_plain, "host wiring")
SST_TABLE(rln, RLN_SST_ROWS)

/* ----- Screen position math (multi-line mode) ----- */

// True when rln must not emit EL or break rows. Two cases:
//   1. Plain lines (prompt_col == 0). No row math possible, and
//      callers may be inside a form whose adjacent data we mustn't clobber.
//   2. Geometry known and the configured max_length fits on the prompt
//      row, so the input cannot wrap. Same form-data concern applies.
// Wrap mode (returns false) is only when prompt_col is known AND the
// input genuinely can take a second row — the multi-row regime where
// row breaks and per-row EL are appropriate and necessary.
static bool rln_input_no_wrap(void)
{
    if (!rln_prompt_col)
        return true;
    uint16_t w = rln_width;
    return rln_prompt_col - 1 + rln_max_length <= w;
}

static void rln_buf_to_screen(uint8_t i, uint8_t *row, uint16_t *col)
{
    uint16_t w = rln_width;
    uint32_t logical = (uint32_t)(rln_prompt_col - 1) + i;
    *row = (uint8_t)(logical / w);
    *col = (uint16_t)(logical % w) + 1;
}

// A move of the cursor up, down or right by n ('A', 'B' or 'C'), and an
// erase to the end of the line, each as the console's terminal takes it now.
// A VT52 has them as ESC and the letter, with no count, so a move by n is
// the command n times.
static void rln_emit_move(char way, unsigned n)
{
    if (!n)
        return;
    if (!cells_term.vt52)
    {
        char seq[12];
        snprintf(seq, sizeof seq, "\33[%u%c", n, way);
        rln_out_s(seq);
    }
    else
        rln_out_rep(RLN_OUT_ESC, way, n);
}

// Move to column col of the cursor's row: CR, then right. VT100 and VT102
// have no CHA. CR goes to the real cell from a pending wrap at the margin,
// and clears it.
static void rln_emit_col(uint16_t col)
{
    rln_out_c('\r');
    rln_emit_move('C', col - 1u);
}

// Erase the line's row from the cursor, which is at column col and stays
// there. The terminal's erase goes to the end of the row whatever the right
// margin is, so a line that ends at a margin erases with spaces, and comes
// back.
static void rln_emit_erase(uint16_t col)
{
    if (rln_erase_el)
    {
        rln_out_s(cells_term.vt52 ? "\33K" : "\33[K");
        return;
    }
    if (col <= rln_width)
        rln_out_rep(RLN_OUT_REP, ' ', rln_width - col + 1u);
    rln_emit_col(col);
}

// Move the visible cursor so that it sits at the screen position
// corresponding to buffer index `target`.
static void rln_sync_cursor_to(uint8_t target)
{
    // Plain lines have no prompt column, so no row math: BS goes left,
    // printing the characters already there goes right, and the terminal
    // wraps.
    if (rln_prompt_col == 0)
    {
        if (rln_cur_idx > target)
            rln_out_rep(RLN_OUT_REP, '\b', rln_cur_idx - target);
        for (uint8_t i = rln_cur_idx; i < target; i++)
            rln_out_buf(i);
    }
    else
    {
        // Rows by CUU and CUD, which never scroll: rln drew every row the
        // line has. The column by CR and CUF. A move that goes nowhere sends
        // nothing, except on a form's row, which is placed every time: its
        // erase spaces leave the cursor past the cell rln counts it at, and
        // a write to its last column parks there.
        uint8_t fr, tr;
        uint16_t fc, tc;
        rln_buf_to_screen(rln_cur_idx, &fr, &fc);
        rln_buf_to_screen(target, &tr, &tc);
        if (tr > fr)
            rln_emit_move('B', (unsigned)(tr - fr));
        else if (tr < fr)
            rln_emit_move('A', (unsigned)(fr - tr));
        if (tr != fr || tc != fc || rln_input_no_wrap())
            rln_emit_col(tc);
    }
    rln_cur_idx = target;
}

// Write ch at buffer index i. A write to the last column leaves the cursor
// parked on it. In a line that can take more rows, CR LF then breaks the row
// when the cursor is to reach index i + 1 at or before end; otherwise CR and
// CUF end the park, so the next write cannot wrap.
static void rln_put(uint8_t i, char ch, uint8_t end)
{
    if (ch == rln_buf[i])
        rln_out_buf(i);
    else
        rln_out_c(ch);
    rln_cur_idx = (uint8_t)(i + 1);
    if (!rln_prompt_col)
        return;
    uint8_t r;
    uint16_t c;
    rln_buf_to_screen(i, &r, &c);
    if (c != rln_width)
        return;
    if (rln_input_no_wrap())
        rln_cur_idx = i;
    else if (i + 1 <= end)
        rln_out_s("\r\n");
    else
    {
        rln_cur_idx = i;
        rln_emit_col(c);
    }
}

// Effective max chars we can hold, given current screen geometry. In
// multi-line mode we cap at rln_rows visible rows.
static uint8_t rln_effective_max(void)
{
    uint8_t cap = rln_max_length;
    if (rln_prompt_col)
    {
        // The prompt column is on the prompt's row, so this is at least 1.
        unsigned avail = rln_rows * rln_width - (rln_prompt_col - 1u);
        if (cap > avail)
            cap = (uint8_t)avail;
    }
    return cap;
}

// Highest reachable cursor index. Normally buflen; one less when the
// input is full and fills exactly to the last column, so the cursor
// rests on the last cell instead of breaking to a row the line can never
// use. The "full" guard keeps multi-line appending working — merely
// filling a row must still let you move on.
static uint8_t rln_cursor_max(void)
{
    if (rln_buflen && rln_prompt_col &&
        rln_buflen >= rln_effective_max() &&
        ((rln_prompt_col - 1u + rln_buflen) % rln_width) == 0)
        return (uint8_t)(rln_buflen - 1);
    return rln_buflen;
}

static void rln_clamp_cursor(void)
{
    uint8_t m = rln_cursor_max();
    if (rln_bufpos > m)
        rln_bufpos = m;
}

// Cut the line to what the layout can hold, so buflen <= effective_max
// always holds (overwrite edits in place without a cap check).
static void rln_fit(void)
{
    uint8_t cap = rln_effective_max();
    if (rln_buflen > cap)
    {
        rln_buflen = cap;
        rln_buf[rln_buflen] = 0;
    }
}

// Move the cursor to the end of the line, or past a caret drawn beyond it.
static void rln_sync_past_line(void)
{
    uint8_t end = rln_cursor_max();
    if (rln_cur_idx > end)
        end = rln_cur_idx;
    rln_sync_cursor_to(end);
}

// True when the cursor is in column 1 of the row below the line, where
// rln_put broke the row after the line's last column.
static bool rln_below_line(void)
{
    if (!rln_prompt_col || !rln_cur_idx)
        return false;
    uint8_t r;
    uint16_t c;
    rln_buf_to_screen(rln_cur_idx, &r, &c);
    return c == 1;
}

// Write `count` erase spaces from the cell just past rln_buflen. A form's
// row is placed by column after, but a plain line moves relatively, so it
// backs up to where the spaces started.
static void rln_emit_trailing_spaces(uint8_t count)
{
    rln_out_rep(RLN_OUT_REP, ' ', count);
    if (!rln_prompt_col)
        rln_out_rep(RLN_OUT_REP, '\b', count);
}

// Walk down `rows` existing rows from the cursor, clearing each, then return up
// to the start row at column `col`.
static void rln_clear_rows_below(uint8_t rows, uint16_t col)
{
    for (uint8_t k = 0; k < rows; k++)
    {
        rln_out_s(cells_term.vt52 ? "\33B\r" : "\33[B\r");
        rln_emit_erase(1);
    }
    rln_emit_move('A', rows);
    if (rows)
        rln_emit_move('C', col - 1u);
}

// Redraw the buffer, then place the cursor at rln_bufpos. Two paths:
//   - No-wrap: plain writes from `start` with space-fill erase. No EL — it
//     would clobber adjacent form data on the same row.
//   - Wrap: write from `start`, breaking the rows, then clear the tail and
//     any rows a previous longer render owned. Never \33[J, which would
//     erase rows below that rln does not own.
// Both erase an old tail only when the line got shorter, so typing at the end
// of the line echoes just the typed char.
static void rln_render_from(uint8_t start)
{
    rln_clamp_cursor();
    rln_sync_cursor_to(start);
    if (rln_input_no_wrap())
    {
        for (uint8_t i = start; i < rln_buflen; i++)
            rln_out_buf(i);
        rln_cur_idx = rln_cursor_max();
        if (rln_last_render_buflen > rln_buflen)
            rln_emit_trailing_spaces((uint8_t)(rln_last_render_buflen - rln_buflen));
        rln_last_render_buflen = rln_buflen;
    }
    else
    {
        uint8_t end = rln_cursor_max();
        for (uint8_t i = start; i < rln_buflen; i++)
            rln_put(i, rln_buf[i], end);
        // The cursor is past the line, or on its last cell when a full line
        // ends on the last column, where EL would erase that cell.
        uint8_t er;
        uint16_t ec;
        rln_buf_to_screen(rln_cur_idx, &er, &ec);
        if (rln_cur_idx == rln_buflen && rln_last_render_buflen > rln_buflen)
            rln_emit_erase(ec);
        // Clear rows a previous (longer) render owned below the new end,
        // then return to the new end so cur_idx tracks the screen.
        if (rln_rendered_max_row > er)
            rln_clear_rows_below((uint8_t)(rln_rendered_max_row - er), ec);
        rln_rendered_max_row = er;
        rln_last_render_buflen = rln_buflen;
    }
    rln_sync_cursor_to(rln_bufpos);
}

/* ----- Tail-rewrite emit helper ----- */

// Emit the screen update for an in-place buffer change at position `at`.
// The buffer must already reflect the new state; leaves cursor at
// rln_bufpos. `pad` = trailing spaces to erase old positions past the new
// tail (0 for insert/in-place replace, N for delete-N). Wrap mode falls
// back to rln_render_from.
static void rln_emit_tail_rewrite(uint8_t at, uint8_t pad)
{
    rln_clamp_cursor();
    if (!rln_input_no_wrap())
    {
        rln_render_from(at);
        return;
    }
    rln_sync_cursor_to(at);
    for (uint8_t i = at; i < rln_buflen; i++)
        rln_out_buf(i);
    if (pad)
        rln_emit_trailing_spaces(pad);
    rln_cur_idx = rln_cursor_max();
    rln_last_render_buflen = rln_buflen;
    rln_sync_cursor_to(rln_bufpos);
}

/* ----- History ----- */

static void rln_replace_buf_from_history(void)
{
    rln_buflen = (uint8_t)strnlen(rln_buf, RLN_BUF_SIZE - 1);
    rln_fit();
    rln_bufpos = rln_buflen;
    rln_render_from(0);
}

// dir > 0 walks toward older entries (up arrow), dir < 0 toward newer.
static void rln_history_step(int dir)
{
    rln_history_t *h = rln_history;
    if (!h)
        return;
    if (dir > 0 && h->pos >= h->count)
        return;
    if (dir < 0 && !h->pos)
        return;
    rln_buf[rln_buflen] = 0;
    memcpy(h->line[h->pos], rln_buf, rln_buflen + 1);
    h->pos += dir;
    memcpy(rln_buf, h->line[h->pos], RLN_BUF_SIZE);
    rln_action_taken = true;
    rln_replace_buf_from_history();
}

// The line entered becomes the newest, unless it is empty or the newest
// again, and the oldest is dropped from a history that is full.
static void rln_history_add(void)
{
    rln_history_t *h = rln_history;
    if (!h || h->lines < 2 || rln_buflen == 0)
        return;
    rln_buf[rln_buflen] = 0;
    if (h->count > 0 && strcmp(h->line[1], rln_buf) == 0)
        return;
    memmove(h->line[2], h->line[1], (size_t)(h->lines - 2) * RLN_BUF_SIZE);
    memcpy(h->line[1], rln_buf, rln_buflen + 1);
    h->line[0][0] = 0;
    if (h->count < h->lines - 1)
        h->count++;
}

// The cursor shape as DECSCUSR's Ps: 1 a block for overwrite, 5 a bar for
// insert, 0 the default. A terminal on the UART or telnet may print it, as
// minicom prints its 'q', so it goes to the screen alone and the mode shows
// nowhere else. Plain lines get none, and a terminal in VT52 mode takes no
// CSI.
static void rln_cursor_shape(uint8_t ps)
{
    if (!rln_prompt_col || cells_term.vt52)
        return;
    char seq[] = "\33[0 q";
    seq[2] = (char)('0' + ps);
    rln_out_screen(seq, sizeof seq - 1);
}

// The read is over, or given up: nothing more is read for it, the callback
// never fires, and the cursor has its default shape again.
void rln_read_cancel(void)
{
    rln_callback = NULL;
    rln_idle_timeout_ms = 0;
    tio_read_end();
    rln_cursor_shape(0);
    rln_out_send(false);
}

/* The line is done at Enter: nothing is read after it, so whatever follows
 * is the next reader's. The newline takes the cursor to column 1 of the row
 * below the line, unless the line's own row break already did. SUPPRESS_NL
 * drops it so last-line field input keeps the cursor on its row. */
static void rln_complete(bool timed_out)
{
    rln_read_callback_t cc = rln_callback;
    rln_read_cancel();
    if (!rln_suppress_newline && !rln_below_line())
        rln_out_c('\n');
    rln_out_then = cc;
    rln_out_timed_out = timed_out;
    rln_out_send(false);
}

// Park the cursor past the input, nul-terminate the buffer, optionally
// push to history, and complete. The terminating newline is emitted in
// rln_complete (suppressed by the SUPPRESS_NL attribute).
static void rln_finish_line(bool add_to_history)
{
    rln_sync_past_line();
    rln_buf[rln_buflen] = 0;
    if (add_to_history)
        rln_history_add();
    rln_complete(false);
}

/* ----- Movement helpers ----- */

static void rln_line_home(void)
{
    if (rln_bufpos)
        rln_action_taken = true;
    rln_bufpos = 0;
    rln_sync_cursor_to(rln_bufpos);
}

static void rln_line_end(void)
{
    uint8_t e = rln_cursor_max();
    if (rln_bufpos != e)
        rln_action_taken = true;
    rln_bufpos = e;
    rln_sync_cursor_to(rln_bufpos);
}

static bool rln_is_word_delimiter(char ch)
{
    return ch == ' ' || ch == '/' || ch == '\\' || ch == '.' || ch == ':' || ch == '=';
}

// Scan from rln_bufpos to the next word boundary in the given direction.
// dir > 0: end of current/next word; dir < 0: start of current/previous
// word. Returns the new buffer index (clamped to [0, rln_buflen]); equals
// rln_bufpos when already at the boundary.
static uint8_t rln_scan_word(int dir)
{
    uint8_t pos = rln_bufpos;
    if (dir > 0)
    {
        while (pos < rln_buflen)
        {
            pos++;
            if (pos >= rln_buflen)
                break;
            if (rln_is_word_delimiter(rln_buf[pos]) &&
                !rln_is_word_delimiter(rln_buf[pos - 1]))
                break;
        }
    }
    else
    {
        while (pos)
        {
            pos--;
            if (!pos)
                break;
            if (!rln_is_word_delimiter(rln_buf[pos]) &&
                rln_is_word_delimiter(rln_buf[pos - 1]))
                break;
        }
    }
    return pos;
}

static void rln_line_word(int dir)
{
    uint8_t to = rln_scan_word(dir);
    if (to == rln_bufpos)
        return;
    rln_bufpos = to;
    rln_clamp_cursor();
    rln_action_taken = true;
    rln_sync_cursor_to(rln_bufpos);
}

static void rln_step(int delta)
{
    uint8_t prev = rln_bufpos;
    if (delta > 0)
    {
        uint16_t room = (uint16_t)(rln_buflen - rln_bufpos);
        if ((uint16_t)delta > room)
            delta = (int)room;
        rln_bufpos += (uint8_t)delta;
    }
    else if (delta < 0)
    {
        uint16_t mag = (uint16_t)(-delta);
        if (mag > rln_bufpos)
            mag = rln_bufpos;
        rln_bufpos -= (uint8_t)mag;
    }
    rln_clamp_cursor();
    if (rln_bufpos != prev)
    {
        rln_action_taken = true;
        rln_sync_cursor_to(rln_bufpos);
    }
}

// CSI numeric parameter with the ANSI default-of-1 applied: an omitted or
// zero Ps means 1.
static uint16_t rln_param_or_1(uint16_t v)
{
    return v < 1 ? 1 : v;
}

static void rln_line_forward(const tio_csi_t *csi)
{
    if (csi->count > 1 && csi->param[1] != 1)
        return rln_line_word(1);
    int count = rln_param_or_1(csi->param[0]);
    rln_step(count);
}

static void rln_line_backward(const tio_csi_t *csi)
{
    if (csi->count > 1 && csi->param[1] != 1)
        return rln_line_word(-1);
    int count = rln_param_or_1(csi->param[0]);
    rln_step(-count);
}

/* ----- Buffer mutations ----- */

// Excise rln_buf[start..end) and leave the cursor at start. Caller must
// pass start <= rln_buflen; end is clamped down to rln_buflen so callers
// can pass an unclamped target.
static void rln_delete_range(uint8_t start, uint8_t end)
{
    if (end > rln_buflen)
        end = rln_buflen;
    if (start >= end)
        return;
    uint8_t count = (uint8_t)(end - start);
    memmove(rln_buf + start, rln_buf + end, (size_t)(rln_buflen - end));
    rln_buflen -= count;
    rln_bufpos = start;
    rln_action_taken = true;
    rln_emit_tail_rewrite(start, count);
}

static void rln_line_delete(void)
{
    rln_delete_range(rln_bufpos, rln_bufpos + 1);
}

static void rln_line_delete_n(const tio_csi_t *csi)
{
    uint16_t count = rln_param_or_1(csi->param[0]);
    uint32_t end = (uint32_t)rln_bufpos + count;
    if (end > rln_buflen)
        end = rln_buflen;
    rln_delete_range(rln_bufpos, (uint8_t)end);
}

static void rln_line_backspace(void)
{
    if (rln_bufpos)
        rln_delete_range(rln_bufpos - 1, rln_bufpos);
}

static void rln_line_backward_kill_word(void)
{
    rln_delete_range(rln_scan_word(-1), rln_bufpos);
}

static void rln_line_forward_kill_word(void)
{
    rln_delete_range(rln_bufpos, rln_scan_word(1));
}

static void rln_line_kill_to_end(void)
{
    rln_delete_range(rln_bufpos, rln_buflen);
}

static void rln_line_kill_to_start(void)
{
    rln_delete_range(0, rln_bufpos);
}

// ICH (CSI Ps @): insert Ps blank chars at the cursor, shifting the tail
// right. Cursor stays at its current position. Symmetric with DCH.
static void rln_line_insert_n(const tio_csi_t *csi)
{
    uint16_t count = rln_param_or_1(csi->param[0]);
    uint8_t cap = rln_effective_max();
    uint8_t avail = (cap > rln_buflen) ? (uint8_t)(cap - rln_buflen) : 0;
    if (count > avail)
        count = avail;
    if (!count)
        return;
    memmove(rln_buf + rln_bufpos + count, rln_buf + rln_bufpos,
            (size_t)(rln_buflen - rln_bufpos));
    memset(rln_buf + rln_bufpos, ' ', count);
    rln_buflen += (uint8_t)count;
    rln_action_taken = true;
    rln_emit_tail_rewrite(rln_bufpos, 0);
}

// ECH (CSI Ps X): erase Ps chars at the cursor by replacing them with
// spaces. Cursor stays put, buflen unchanged, no shift. Stops at buflen.
static void rln_line_erase_n(const tio_csi_t *csi)
{
    uint16_t count = rln_param_or_1(csi->param[0]);
    uint32_t end = (uint32_t)rln_bufpos + count;
    if (end > rln_buflen)
        end = rln_buflen;
    if (end <= rln_bufpos)
        return;
    uint8_t at = rln_bufpos;
    uint8_t span = (uint8_t)(end - at);
    memset(rln_buf + at, ' ', span);
    rln_action_taken = true;
    rln_emit_tail_rewrite(at, 0);
}

static void rln_line_insert(char ch)
{
    if ((unsigned char)ch < 32)
        return;
    if (rln_caps == RLN_CAPS_UPPER && islower((unsigned char)ch))
        ch = toupper((unsigned char)ch);
    else if (rln_caps == RLN_CAPS_SWAP)
    {
        if (islower((unsigned char)ch))
            ch = toupper((unsigned char)ch);
        else if (isupper((unsigned char)ch))
            ch = tolower((unsigned char)ch);
    }
    if (rln_overwrite && rln_bufpos < rln_buflen)
    {
        // Overwrite replaces one char in place; nothing past the cursor
        // moves.
        rln_buf[rln_bufpos] = ch;
        rln_action_taken = true;
        rln_sync_cursor_to(rln_bufpos);
        rln_put(rln_bufpos, ch, rln_cursor_max());
        rln_bufpos++;
        rln_clamp_cursor();
        rln_sync_cursor_to(rln_bufpos);
        return;
    }
    if (rln_buflen >= rln_effective_max())
        return;
    memmove(rln_buf + rln_bufpos + 1, rln_buf + rln_bufpos, (size_t)(rln_buflen - rln_bufpos));
    rln_buflen++;
    rln_buf[rln_bufpos] = ch;
    uint8_t at = rln_bufpos;
    rln_bufpos++;
    rln_action_taken = true;
    rln_emit_tail_rewrite(at, 0);
}

/* ----- Mode toggling ----- */

// The mode's shape, while a read is on. A poked Enter can end the read before
// the poke's mode is put back.
static void rln_emit_mode_cursor(void)
{
    if (rln_callback)
        rln_cursor_shape(rln_overwrite ? 1 : 5);
}

static void rln_toggle_overwrite(void)
{
    rln_overwrite = !rln_overwrite;
    rln_action_taken = true;
    rln_emit_mode_cursor();
}

/* ----- Items -----
 *
 * tio hands over one item at a time: a character, or a whole escape
 * sequence. What is typed before a read stays in its source until the read
 * takes it. */

// Enter, a line end the source remembers so the other half of a pair is
// absorbed (rln_item).
static void rln_enter(com_source_t src, uint8_t ch)
{
    rln_line_ends[src] = ch;
    rln_action_taken = true;
    rln_finish_line(true);
}

static void rln_dispatch_C0(com_source_t src, uint8_t ch)
{
    if (ch == '\r' || ch == '\n')
        rln_enter(src, ch);
    else if (ch == '\b' || ch == 127)
        rln_line_backspace();
    else if (ch == 1) // ctrl-a
        rln_line_home();
    else if (ch == 2) // ctrl-b
        rln_step(-1);
    else if (ch == 4) // ctrl-d
        rln_line_delete();
    else if (ch == 5) // ctrl-e
        rln_line_end();
    else if (ch == 6) // ctrl-f
        rln_step(1);
    else if (ch == 11) // ctrl-k
        rln_line_kill_to_end();
    else if (ch == 14) // ctrl-n
        rln_history_step(-1);
    else if (ch == 16) // ctrl-p
        rln_history_step(1);
    else if (ch == 21) // ctrl-u
        rln_line_kill_to_start();
    else if (ch == 23) // ctrl-w
        rln_line_backward_kill_word();
    else
        rln_line_insert(ch);
}

static void rln_dispatch_Fe(uint8_t ch)
{
    // ESC-b / ESC-f are meta-b / meta-f (alt-b/alt-f on xterm-style
    // terminals); ESC 0x02 / ESC 0x06 are the ctrl-B / ctrl-F variants some
    // older meta-prefix terminals send for the same binding. ESC A to D, H
    // and F are a VT52's arrows, Home and End.
    if (ch == 'A')
        rln_history_step(1);
    else if (ch == 'B')
        rln_history_step(-1);
    else if (ch == 'C')
        rln_step(1);
    else if (ch == 'D')
        rln_step(-1);
    else if (ch == 'F')
        rln_line_end();
    else if (ch == 'H')
        rln_line_home();
    else if (ch == 'b' || ch == 2)
        rln_line_word(-1);
    else if (ch == 'f' || ch == 6)
        rln_line_word(1);
    else if (ch == 'd')
        rln_line_forward_kill_word();
    else if (ch == 127 || ch == '\b')
        rln_line_backward_kill_word();
}

// SS3 keys, which a terminal sends in application cursor or keypad mode:
// the arrows, Home and End, and the keypad, Enter typing a line end and the
// rest their characters. A key with parameters, such as ESC O 5 P, does
// nothing.
static void rln_dispatch_SS3(com_source_t src, const char *item, size_t len)
{
    if (len != 3)
        return;
    uint8_t ch = (uint8_t)item[2];
    if (ch == 'A')
        rln_history_step(1);
    else if (ch == 'B')
        rln_history_step(-1);
    else if (ch == 'C')
        rln_step(1);
    else if (ch == 'D')
        rln_step(-1);
    else if (ch == 'F')
        rln_line_end();
    else if (ch == 'H')
        rln_line_home();
    else if (ch == 'M')
        rln_enter(src, '\r');
    else if (ch == 'X')
        rln_line_insert('=');
    else if (ch >= 'j' && ch <= 'y')
        rln_line_insert("*+,-./0123456789"[ch - 'j']);
}

// DCH, ICH and ECH are editing commands a program pokes. Typed, they are
// function keys, Shift-F1 being CSI 1;2P, and do nothing. rxvt's shifted
// keys end on '$' where others end on '~'.
static void rln_dispatch_CSI(com_source_t src, const tio_csi_t *csi)
{
    uint8_t ch = csi->final;
    if (src == RLN_POKE && ch == 'P')
        rln_line_delete_n(csi);
    else if (src == RLN_POKE && ch == '@')
        rln_line_insert_n(csi);
    else if (src == RLN_POKE && ch == 'X')
        rln_line_erase_n(csi);
    else if (ch == 'A')
        rln_history_step(1);
    else if (ch == 'B')
        rln_history_step(-1);
    else if (ch == 'C')
        rln_line_forward(csi);
    else if (ch == 'D')
        rln_line_backward(csi);
    else if (ch == 'F')
        rln_line_end();
    else if (ch == 'H')
        rln_line_home();
    else if (ch == 'b')
        rln_line_word(-1);
    else if (ch == 'f')
        rln_line_word(1);
    else if (ch == '~' || ch == '$')
        // Function-key codes split across terminfo lineages: 1/4 = VT220-era
        // Find/Select repurposed as Home/End; 7/8 = rxvt/Linux console; 2/3
        // = Insert/Delete (universal). Keep both Home/End forms.
        switch (csi->param[0])
        {
        case 1:
        case 7:
            rln_line_home();
            break;
        case 2:
            rln_toggle_overwrite();
            break;
        case 4:
        case 8:
            rln_line_end();
            break;
        case 3:
            rln_line_delete();
            break;
        }
}

// Saves the key just dispatched for RLN_LASTKEY.
static void rln_publish_lastkey(const char *item, size_t len)
{
    memcpy(rln_lastkey_buf, item, len);
    rln_lastkey_len = (uint8_t)len;
    rln_lastkey_action = rln_action_taken;
}

// A typed item, dispatched while editing. A poked item is dispatched in
// overwrite mode, and a shape it changes is put back.
static void rln_dispatch(com_source_t src, const char *item, size_t len,
                         const tio_csi_t *csi)
{
    bool was_overwrite = rln_overwrite;
    if (src == RLN_POKE)
        rln_overwrite = true;
    rln_action_taken = false;
    rln_line_src = src;
    if (len == 1)
        rln_dispatch_C0(src, (uint8_t)item[0]);
    else if (item[1] == '[')
        rln_dispatch_CSI(src, csi);
    else if (item[1] == 'O')
        rln_dispatch_SS3(src, item, len);
    else
        rln_dispatch_Fe((uint8_t)item[1]);
    rln_publish_lastkey(item, len);
    if (src == RLN_POKE)
    {
        bool toggled = !rln_overwrite;
        rln_overwrite = was_overwrite;
        if (toggled && was_overwrite)
            rln_emit_mode_cursor();
    }
}

// Takes one item from a source, or from the poke stream. Private CSIs, CSIs
// with an intermediate, such as a terminal's mode report CSI 4;2$y, CSIs that
// do not parse, and SS2 are dropped, and the rest are typed. No dropped item
// becomes the last key.
//
// Both CR and LF end a line, because a terminal sends CR and a file of host
// text holds LF, and nothing rewrites either on the way in. A CR and LF next
// to each other from one source are one line ending, so the second is
// absorbed here, even when it is the next read's first item. The memory
// lasts exactly one item, which is why "\r\r" is two lines and why a blank
// line in a CRLF file survives.
static void rln_item(com_source_t src, const char *item, size_t len)
{
    tio_csi_t csi;
    bool is_csi = tio_csi_parse(item, len, &csi);
    if ((is_csi && (csi.priv || csi.inter)) ||
        (len > 1 && item[1] == '[' && !is_csi) ||
        (len == 3 && item[1] == 'N'))
        return;
    uint8_t was = rln_line_ends[src];
    rln_line_ends[src] = 0;
    if (len == 1 && was && (item[0] == '\r' || item[0] == '\n') && item[0] != was)
        return;
    rln_dispatch(src, item, len, &csi);
}

/* ----- Following the terminal ----- */

// The line starts where the console's terminal has its cursor, and is laid
// out at that row's columns, half the width on a double-size row, and those
// between the left and right margins for a cursor between them. Everything
// already printed reaches the terminals and cells_term first, then the modes
// go to every terminal. A cursor in a pending wrap gets a CR LF, so the line
// starts in column 1 of the next row. A terminal in VT52 mode has no insert
// mode to turn off, and is sent a VT52's form of every move and erase.
static void rln_place(void);

static void rln_lay_out(void)
{
    manifold_flush();
    if (term_pos(term_shown(&cells_term))->wrap_pending)
    {
        manifold_putchar('\r');
        manifold_putchar('\n');
        manifold_flush();
    }
    rln_place();
}

// The layout of a read as it starts, without waiting inside: until what was
// written has reached the terminal, it is left for rln_task to try again on
// a later pass, a CR LF for a pending wrap waiting the same way. True once
// the line is laid out.
static bool rln_lay_out_now(void)
{
    if (!manifold_drained())
        return false;
    if (term_pos(term_shown(&cells_term))->wrap_pending)
    {
        manifold_putchar('\r');
        manifold_putchar('\n');
        if (!manifold_drained())
            return false;
    }
    rln_place();
    return true;
}

// The modes go to every terminal and the line takes the cursor's place.
static void rln_place(void)
{
    bool plain = rln_plain;
    if (!plain && !cells_term.vt52)
        rln_out_s(RLN_MODES);
    /* The line has the columns the terminal gives the cursor: from where a
     * CR lands to where a row of text ends, which are the left and right
     * margins for a cursor between them. Its columns are counted from the
     * first of those. It has no more rows than the scroll region the
     * cursor is in, where its row breaks scroll. */
    const term_grid_t *g = term_shown(&cells_term);
    const term_pos_t *p = term_pos(g);
    const unsigned first = term_left_stop(g);
    rln_prompt_col = plain ? 0 : (uint16_t)(p->x - first + 1);
    rln_width = (uint16_t)(term_right_stop(g) - first + 1);
    rln_erase_el = term_right_stop(g) == term_cols(g, p->y) - 1;
    rln_grid_width = g->width;
    unsigned rows = g->height;
    if (p->y >= g->margin_top && p->y <= g->margin_bot)
        rows = g->margin_bot - g->margin_top + 1u;
    rln_rows = (uint8_t)(rows < RLN_MAX_ROWS ? rows : RLN_MAX_ROWS);
    rln_emit_mode_cursor();
}

// The line is drawn again when the shown grid's width changes under it: a new
// canvas, a VGA reset or a program's own width. No terminal is trusted to have
// reflowed it, so it starts over on a clean row below the old line, at the
// new width, without the prompt, cut to what the new layout holds. A plain
// line is never drawn again.
static void rln_follow(void)
{
    if (!rln_prompt_col || rln_get_term_width() == rln_grid_width)
        return;
    rln_sync_past_line();
    rln_out_send(true);
    com_clean_line();
    rln_lay_out();
    rln_fit();
    rln_cur_idx = 0;
    rln_rendered_max_row = 0;
    rln_last_render_buflen = 0;
    rln_render_from(0);
}

/* ----- Top-level task / driver hooks ----- */

static void rln_start(rln_read_callback_t callback, rln_history_t *history)
{
    /* The reply to the last read's last key goes out first. A read started
     * before that read was handed its line replaces it, and the line is
     * dropped: its readers wait for rln_replying to end before they ask for
     * another. */
    rln_out_then = NULL;
    rln_out_send(true);
    rln_idle_timeout_ms = 0;
    rln_buflen = 0;
    rln_bufpos = 0;
    rln_callback = callback;
    rln_history = history;
    if (history)
        history->pos = 0;
    rln_buf[0] = 0;
    rln_lastkey_len = 0;
    rln_action_taken = false;
    rln_lastkey_action = false;

    rln_prompt_col = 0;
    rln_cur_idx = 0;
    rln_rendered_max_row = 0;
    rln_last_render_buflen = 0;
    rln_line_src = COM_SOURCE_ANY;
    rln_laying_out = false;
}

void rln_read_line(rln_read_callback_t callback, rln_history_t *history)
{
    rln_start(callback, history);
    rln_laying_out = !rln_lay_out_now();
    rln_out_send(false);
}

void rln_read_line_script(rln_read_callback_t callback, uint32_t timeout_ms)
{
    assert(timeout_ms);
    rln_start(callback, NULL);
    rln_idle_timeout_ms = timeout_ms;
    rln_idle_deadline = timer_mach_in_ms(rln_idle_timeout_ms);
}

com_source_t rln_line_source(void)
{
    return rln_line_src;
}

bool rln_read_flush(void)
{
    if (!rln_callback || !rln_buflen)
        return false;
    rln_line_src = COM_SOURCE_ANY;
    rln_finish_line(false);
    rln_out_send(false);
    return true;
}

void HOST_IN_RAM_FUNC rln_task(void)
{
    if (!rln_out_send(false) || !rln_callback)
        return;
    if (rln_laying_out && (rln_laying_out = !rln_lay_out_now()))
        return;
    char item[TIO_ITEM_MAX];
    com_source_t src;
    size_t len;
    for (;;)
    {
        if (rln_callback)
            rln_follow();
        if (!rln_callback || !(len = tio_read(&src, item)))
            break;
        rln_idle_deadline = timer_mach_in_ms(rln_idle_timeout_ms);
        rln_item(src, item, len);
        if (!rln_out_send(false))
            return;
    }
    if (rln_idle_timeout_ms && timer_mach_passed(rln_idle_deadline))
    {
        rln_line_src = COM_SOURCE_ANY;
        rln_complete(true);
    }
}

void HOST_IN_FLASH_FUNC rln_init(void)
{
    rln_callback = NULL;
    rln_max_length = 255; // fits in 2^8 with nul
    rln_caps = RLN_CAPS_OFF;
    rln_suppress_newline = false;
    rln_overwrite = false;
    rln_pinned_width = 0;
    rln_pinned_height = 0;
    term_grid_init(&rln_grid, 80, rln_grid_lines, RLN_GRID_ROWS);
}

void rln_run(void)
{
    rln_max_length = 254; // reserve 1 for the stdin newline
}

/* A read cut short by a stop or a break leaves the cursor on a new line
 * past everything typed, with its default shape. A program's own size goes
 * back to the screen's. */
void rln_stop(void)
{
    if (rln_callback)
    {
        rln_sync_past_line();
        if (!rln_below_line())
            rln_out_s("\r\n");
        rln_read_cancel();
    }
    rln_out_then = NULL;
    rln_out_send(true);
    rln_reset();
}

/* No read, and the third grid off the screen's terminal, with nothing drawn:
 * after a failed load the line's indexes and geometry may hold anything, and
 * the console they would draw through is not reset yet. CELLS' RESET runs
 * first, so the grid list is sound to search. The rest is set again by the
 * next read's start. */
void rln_reset(void)
{
    if (rln_has_grid())
        term_drop_grid(&cells_term, &rln_grid);
    rln_out_len = rln_out_lit_len = rln_out_at = 0;
    rln_out_sent = 0;
    rln_out_then = NULL;
    rln_init();
}

void rln_set_plain(bool plain)
{
    rln_plain = plain;
}

uint16_t rln_get_term_width(void)
{
    return term_shown(&cells_term)->width;
}

uint16_t rln_get_term_height(void)
{
    return term_shown(&cells_term)->height;
}

/* The third grid starts as a copy of the showing one and takes the pinned
 * size. An axis not pinned takes the showing grid's when the other is
 * pinned, and keeps it: the program's terminal is a fixed size until both
 * are 0 again, when it is dropped. The screen's grids are never touched. */
static void rln_size_grid(void)
{
    term_t *t = &cells_term;
    if (!rln_pinned_width && !rln_pinned_height)
    {
        if (rln_has_grid())
            term_drop_grid(t, &rln_grid);
        return;
    }
    const term_grid_t *shown = cells_shown(t);
    if (!rln_has_grid())
        term_add_grid(t, &rln_grid, shown, rln_grid_lines, RLN_GRID_ROWS);
    term_set_width(t, &rln_grid, (uint8_t)(rln_pinned_width ? rln_pinned_width : shown->width));
    term_set_height(t, &rln_grid, (uint8_t)(rln_pinned_height ? rln_pinned_height : shown->height));
}

void rln_set_term_width(uint16_t v)
{
    rln_pinned_width = v;
    rln_size_grid();
}

void rln_set_term_height(uint16_t v)
{
    rln_pinned_height = v;
    rln_size_grid();
}

void HOST_IN_RAM_FUNC rln_forget_source(com_source_t src)
{
    rln_line_ends[src] = 0;
    tio_forget(src);
}

/* 6502 applications may configure the max length */

void rln_set_max_length(uint8_t v) { rln_max_length = v; }
uint8_t rln_get_max_length(void) { return rln_max_length; }

void rln_set_caps(uint8_t v) { rln_caps = v; }
uint8_t rln_get_caps(void) { return rln_caps; }

void rln_set_suppress_nl(uint8_t v) { rln_suppress_newline = v; }
uint8_t rln_get_suppress_nl(void) { return rln_suppress_newline; }

/* ----- API calls ----- */

// int ria_rln_lastkey(char *key, unsigned char *action);
bool HOST_IN_FLASH_FUNC rln_api_lastkey(void)
{
    if (xstack_ptr != XSTACK_SIZE)
        return api_return_errno(API_EINVAL);
    uint8_t len = 0;
    uint8_t action = 0;
    if (rln_callback && rln_lastkey_len)
    {
        len = rln_lastkey_len;
        action = rln_lastkey_action ? 1 : 0;
        rln_lastkey_len = 0;
        rln_lastkey_action = false;
    }
    for (int i = len - 1; i >= 0; i--)
        api_push_uint8(&rln_lastkey_buf[i]);
    if (len)
        api_push_uint8(&action);
    return api_return_ax(len);
}

// int ria_rln_peek(char *peek, unsigned char *pos);
bool HOST_IN_FLASH_FUNC rln_api_peek(void)
{
    if (xstack_ptr != XSTACK_SIZE || !rln_callback)
        return api_return_errno(API_EINVAL);
    if (!rln_buflen)
        return api_return_ax(0);
    rln_buf[rln_buflen] = 0;
    for (int i = rln_buflen - 1; i >= 0; i--)
        api_push_char(&rln_buf[i]);
    api_push_uint8(&rln_bufpos);
    return api_return_ax(rln_buflen);
}

// Emit a "^X" caret-notation marker for the C0 byte `ch` at buffer
// position `target`. Caller must have already called
// rln_sync_cursor_to(target). A '^' on the last column breaks the row, so
// the marker stays whole.
static void rln_poke_emit_caret(uint8_t target, char ch)
{
    rln_put(target, '^', (uint8_t)(target + 1));
    rln_put((uint8_t)(target + 1), (char)('@' + ch), (uint8_t)(target + 1));
}

// The string is framed into items as a source is. A sequence still open at the
// end of the string is dropped, and so is an overlong one. CAN and SUB cancel
// a sequence they land in; any other control ends the line.
void rln_poke(const char *str)
{
    if (!rln_callback)
        return;
    /* What a typed key's reply still has to send reads the line as it is
     * now, so it goes out before the poke edits it. */
    rln_out_send(true);
    char item[TIO_ITEM_MAX];
    size_t len = 0;
    uint8_t frame = 0;
    // An empty string sends 0x00.
    const char *p = str;
    do
    {
        /* A poked Enter ends the read, and the rest is for no line. */
        if (!rln_callback)
            break;
        uint8_t ch = (uint8_t)*p;
        if (ch < 32 && ch != '\33' && !(frame && (ch == '\30' || ch == '\32')))
        {
            if (ch != '\r' && rln_max_length >= 2)
            {
                uint8_t target = rln_bufpos;
                if (target > (uint8_t)(rln_max_length - 2))
                    target = (uint8_t)(rln_max_length - 2);
                rln_sync_cursor_to(target);
                rln_poke_emit_caret(target, (char)ch);
            }
            rln_line_src = RLN_POKE;
            rln_finish_line(ch == '\r');
            break;
        }
        tio_step_t step = tio_frame(&frame, ch);
        if (step == TIO_AGAIN)
        {
            len = 0;
            step = tio_frame(&frame, ch);
        }
        if (len < TIO_ITEM_MAX)
            item[len] = (char)ch;
        len++;
        if (step == TIO_END && len <= TIO_ITEM_MAX)
            rln_item(RLN_POKE, item, len);
        if (step != TIO_OPEN)
            len = 0;
    } while (*++p);
    rln_out_send(false);
}

// int ria_rln_poke(const char *poke);
bool HOST_IN_FLASH_FUNC rln_api_poke(void)
{
    const char *poke = api_pop_str_end(XSTACK_SIZE);
    if (!poke)
        return api_return_errno(API_EINVAL);
    rln_poke(poke);
    return api_return_ax(0);
}
