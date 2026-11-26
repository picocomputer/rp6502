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
#include "core/hid/clip.h"
#include "core/hid/typed.h"
#include "core/hid/vtkey.h"
#include "core/sys/driver.h"
#include "core/term/grid.h"
#include "core/term/vterm.h"
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
**   - The selection's highlight is the console's terminal's selection
**     (vterm_select), between the line's columns, which mode 0 draws over
**     the cells: nothing is sent for it, and it shows on the screen alone,
**     the one terminal whose rows rln knows.
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

/* The read's kill buffer, or none, and whether the key before this one
 * killed, when this one's kill joins that one's: before it for a kill that
 * reaches back from the cursor, after it for one that reaches on. */
static rln_kill_t *rln_kill;
static bool rln_kill_joins;
static bool rln_killed;

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

/* Where the line is on the screen, for the mouse and the highlight, set as
 * the line is laid out, with everything written before it on the console's
 * terminal, so it is exact on every machine: the row of the screen its
 * first row is on, the column of that row's own its columns count from,
 * the screen it is on, 0 the primary and 1 the alternate, or none for a
 * line laid out on a program's own size, the row whose row break scrolls
 * the line's rows up, or none, and the terminal's RIS count then, since a
 * RIS after it erases the line, so a click there is no longer in it.
 * rln_put follows the line's own scrolls; output written while it is read
 * is not followed. */
#define RLN_NO_ROW UINT8_MAX
static uint8_t rln_row;
static uint8_t rln_left;
static uint8_t rln_screen;
static uint8_t rln_bot;
static uint16_t rln_ris_count;

/* The selection: from the anchor, where it started, to its end, none when
 * they are the same. Its highlight is the console's terminal's selection,
 * which output over a selected cell drops, so it goes on only once the
 * reply that drew the line has reached that terminal: until then it is
 * due. */
static uint8_t rln_sel_anchor;
static uint8_t rln_sel_end;
static bool rln_hl_due;

/* The selection as the key being dispatched found it, for the key to act
 * on: a typed character and Ctrl-Y replace it, Delete and Backspace
 * delete it, and the keys that kill a word kill it. Set and spent inside
 * one dispatch. */
static uint8_t rln_edit_from;
static uint8_t rln_edit_to;

/* The console's mouse in the line, as the line editor has taken it from
 * the keyboard's queue (rln_entry): the end the drag holds, where the
 * press was, and the end it moves, where the drag is, as indexes of the
 * line as it was laid out when the mouse was there, and the unit the press
 * selects by, none when no press has been taken in this read's line. A
 * click, a press by characters that has not dragged, selects nothing: its
 * press is the character under the pointer and its drag the gap nearest
 * the pointer, where the cursor goes in overwrite and in insert mode. */
enum
{
    RLN_UNIT_NONE,
    RLN_UNIT_CHAR,
    RLN_UNIT_WORD,
    RLN_UNIT_LINE,
    RLN_UNIT_CLICK,
};
static uint8_t rln_press_at;
static uint8_t rln_drag_at;
static uint8_t rln_press_unit;

/* A press's entry: the index, whether the pointer is nearer the gap after
 * it, and its place in its run of clicks, of which the second selects by
 * words and the third by lines. */
#define RLN_PRESS_AFTER 0x100
#define RLN_PRESS_CLICKS(e) ((e) >> 9 & 3)

/* Bracketed paste was asked of the terminals for this read. */
static bool rln_bracketing;

/* Each source's bracketed paste: in one that has had no line end yet, or
 * holding that first line end until what follows it shows whether the
 * paste is the one line that waits for Enter (rln_item), with the other
 * half of its pair still to come or taken already. The line end held is in
 * rln_line_ends. A source that sends nothing more is waited for no longer
 * than RLN_PASTE_HOLD_MS. */
enum
{
    RLN_PASTE_NONE,
    RLN_PASTE_FIRST,
    RLN_PASTE_HELD,
    RLN_PASTE_PAIRED,
};
#define RLN_PASTE_HOLD_MS 5000
static timer_mach_t rln_paste_deadline;

// A program's own size for the console's terminal, while either is pinned:
// a third grid the terminal also follows, which only the line editor uses.
static uint16_t rln_pinned_width;
static uint16_t rln_pinned_height;
static term_grid_t rln_grid;
static uint8_t rln_grid_lines[2 * RLN_GRID_ROWS];

// The CR or LF each source, the poke stream included, last ended a line
// with, so the other half of a CRLF pair arriving next is absorbed instead of
// ending a second line.
static uint8_t rln_line_ends[RLN_SOURCE_COUNT];
static uint8_t rln_paste_in[RLN_SOURCE_COUNT];

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
    return vterm.grid_count > TERM_SCREEN_GRIDS;
}

// The terminal the line editor lays its line out on: the program's own size
// while it has pinned one, otherwise the screen's. Only the line editor
// uses the pinned size; the terminal answers queries from the screen.
static term_grid_t *rln_term(void)
{
    return rln_has_grid() ? &rln_grid : vterm_get_shown(&vterm);
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
 * as the wrong reader. Its reads have no history, so none is carried, and
 * their kill buffer is the console's, which the console carries. */
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
    rln_kill = console_rln_kill();
    rln_killed = false;
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
        term_add_grid(&vterm, &rln_grid, vterm_get_shown(&vterm),
                      rln_grid_lines, RLN_GRID_ROWS);
    else if (!grid && rln_has_grid())
        term_drop_grid(&vterm, &rln_grid);
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
    if (rln_press_unit != RLN_UNIT_NONE && !rln_callback)
        return sst_refuse(c, "a press with no line it was in");
    if (rln_sel_anchor > rln_buflen || rln_sel_end > rln_buflen ||
        rln_press_unit > RLN_UNIT_CLICK || (rln_screen > 1 && rln_screen != RLN_NO_ROW))
        return sst_refuse(c, "a selection past its line");
    for (unsigned i = 0; i < RLN_SOURCE_COUNT; i++)
        if (rln_paste_in[i] > RLN_PASTE_PAIRED)
            return sst_refuse(c, "a paste in no state");
    if (rln_paste_in[RLN_POKE])
        return sst_refuse(c, "a paste in what a program pokes");
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
    X(IN_FN, rln_kill, "the console's, which the load sets")            \
    X(IN_FN, rln_killed, "a load ends a run of kills, as a key does")   \
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
    X(U8, rln_row)                                                      \
    X(U8, rln_left)                                                     \
    X(U8, rln_screen)                                                   \
    X(U8, rln_bot)                                                      \
    X(U16, rln_ris_count)                                               \
    X(U8, rln_sel_anchor)                                               \
    X(U8, rln_sel_end)                                                  \
    X(BOOL, rln_hl_due)                                                 \
    X(U8, rln_press_at)                                                 \
    X(U8, rln_drag_at)                                                  \
    X(U8, rln_press_unit)                                               \
    X(BOOL, rln_bracketing)                                             \
    X(BYTES, rln_paste_in)                                              \
    X(U64, rln_paste_deadline, "machine time, so carried whole")        \
    X(VALIDATE, rln_sst_valid)                                          \
    X(FN, rln_sst_put_grid, rln_sst_get_grid,                           \
      1 + TERM_GRID_SST_SIZE(RLN_GRID_ROWS), 3)                         \
    X(IN_FN, rln_grid)                                                  \
    X(IN_FN, rln_grid_lines)                                            \
    X(SKIP, rln_line_src, "read only inside the read's callback")       \
    X(SKIP, rln_edit_from, "set and spent inside one dispatch")         \
    X(SKIP, rln_edit_to, "set and spent inside one dispatch")           \
    X(SKIP, rln_kill_joins, "set and spent inside one dispatch")        \
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
    if (!vterm.vt52)
    {
        char seq[16];
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
        rln_out_s(vterm.vt52 ? "\33K" : "\33[K");
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
// when the cursor is to reach index i + 1 at or before end, scrolling the
// line's rows up a row at the region's bottom; otherwise CR and CUF end the
// park, so the next write cannot wrap.
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
    {
        rln_out_s("\r\n");
        if (rln_row + r == rln_bot)
            rln_row--;
    }
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

// Cut the line to what the layout can hold, so a line laid out again is no
// longer than its rows (overwrite edits in place without a cap check).
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
        rln_out_s(vterm.vt52 ? "\33B\r" : "\33[B\r");
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

/* ----- The line on the screen, and its selection ----- */

/* The line has a place on the screen showing: a read's line laid out with
 * a prompt column on the grid showing, not on a program's own size, on the
 * screen of that grid that shows now, and with no RIS since, which erased
 * it. */
static bool rln_on_screen(void)
{
    if (!rln_callback || rln_laying_out || !rln_prompt_col)
        return false;
    const term_grid_t *g = vterm_get_shown(&vterm);
    return g->width == rln_grid_width && g->shown - g->screen == rln_screen &&
           vterm.ris_count == rln_ris_count;
}

/* The index at row y of the screen, column x of that row's own, the line
 * having a place there. A place outside the line is -1, or with clamp, the
 * line's start for one before it and its end for one after it. */
static int rln_index_of(int y, int x, bool clamp)
{
    const int r = y - rln_row;
    int c = x - rln_left + 1;
    if (!clamp && (r < 0 || c < 1 || c > rln_width))
        return -1;
    if (c < 1)
        c = 1;
    if (c > rln_width + 1)
        c = rln_width + 1;
    const int i = r * (int)rln_width + c - (int)rln_prompt_col;
    if (i < 0)
        return clamp ? 0 : -1;
    if (i > rln_buflen)
        return clamp ? rln_buflen : -1;
    return i;
}

int rln_index_at(uint8_t y, uint8_t x)
{
    return rln_on_screen() ? rln_index_of(y, x, false) : -1;
}

static uint8_t rln_sel_lo(void)
{
    return rln_sel_anchor < rln_sel_end ? rln_sel_anchor : rln_sel_end;
}

static uint8_t rln_sel_hi(void)
{
    return rln_sel_anchor < rln_sel_end ? rln_sel_end : rln_sel_anchor;
}

/* The selection becomes the line from anchor to end, none when they are
 * the same, and its highlight is due (rln_show). There is one selection on
 * the console's terminal at a time: one that starts ends the primary's
 * highlight at once, and one that ends ends its own. One that changes
 * keeps the highlight it had until the new one shows, as the reply that
 * moves the cursor along it goes out. */
static void rln_select(uint8_t anchor, uint8_t end)
{
    const bool had = rln_sel_anchor != rln_sel_end;
    rln_sel_anchor = anchor;
    rln_sel_end = end;
    rln_hl_due = anchor != end;
    if (vterm_selection.shows && (had ? anchor == end : anchor != end))
        vterm_unselect();
}

/* The highlight goes on, the selection's part of the line as the line is
 * laid out on the screen, once the reply that drew the line has reached
 * the console's terminal: nothing waits to go out to the terminals, as
 * everything written before has reached it, the line's own echo, which
 * would otherwise drop it, included. It is the selection's run from its
 * first character to past its last across the rows the line wraps onto,
 * between the margins of the line's columns, so no column outside the
 * margins the line was laid out in is taken. A line with no place on the
 * screen showing, or one whose rows have gone from it, shows none, and the
 * highlight of the selection before goes: the one selection showing while
 * the line has one is the line's. */
static void HOST_IN_FLASH_FUNC rln_show(void)
{
    rln_hl_due = false;
    const uint8_t lo = rln_sel_lo(), hi = rln_sel_hi();
    if (lo < hi && rln_on_screen())
    {
        term_grid_t *g = vterm_get_shown(&vterm);
        const screen_t *s = g->shown;
        uint8_t r0, r1;
        uint16_t c0, c1;
        rln_buf_to_screen(lo, &r0, &c0);
        rln_buf_to_screen((uint8_t)(hi - 1), &r1, &c1);
        const unsigned y0 = rln_row + r0, y1 = rln_row + r1;
        if (y1 < g->height)
        {
            const uint8_t m0 = screen_mem(s, (uint8_t)y0), m1 = screen_mem(s, (uint8_t)y1);
            const uint8_t x0 = (uint8_t)(rln_left + c0 - 1u), x1 = (uint8_t)(rln_left + c1);
            const uint8_t left = rln_left, right = (uint8_t)(rln_left + rln_width);
            /* One already showing is left as it is, so the RIA sends only
             * a change. */
            const screen_selection_t *v = &vterm_selection;
            if (!v->shows || v->screen != s || v->start_row != m0 || v->start_col != x0 ||
                v->end_row != m1 || v->end_col != x1 || v->left != left || v->right != right)
                vterm_select(g, (uint8_t)(s - g->screen), m0, x0, m1, x1, left, right);
            return;
        }
    }
    if (vterm_selection.shows)
        vterm_unselect();
}

/* No selection, its highlight off, and nothing of the mouse still to take
 * from the keyboard's queue. */
void rln_unselect(void)
{
    typed_void_mouse();
    rln_select(0, 0);
}

/* The line's selection is what its highlight shows. When the highlight goes
 * from under it, by a new canvas, a RIS or the line's rows leaving the
 * screen, the selection ends with it and the cursor stays where it is, so
 * no key acts on text that shows no highlight. One still due to show is
 * kept: its highlight has yet to go on. */
static void rln_selection_follow(void)
{
    if (rln_sel_anchor != rln_sel_end && !rln_hl_due && !vterm_selection.shows)
        rln_sel_anchor = rln_sel_end;
}

/* ----- History ----- */

// A line entered, between the one entered before it and the one after.
struct rln_history_line
{
    struct rln_history_line *older;
    struct rln_history_line *newer;
    char text[];
};

// What the read in progress has made of a line it has left: of an entered
// line, or of the line being typed, which is of none.
struct rln_history_edit
{
    struct rln_history_edit *next;
    struct rln_history_line *of;
    char text[];
};

// The edit of a line is given up, if it has one.
static void rln_history_unedit(rln_history_t *h, const struct rln_history_line *of)
{
    for (struct rln_history_edit **link = &h->edits; *link; link = &(*link)->next)
        if ((*link)->of == of)
        {
            struct rln_history_edit *edit = *link;
            *link = edit->next;
            h->free(edit);
            return;
        }
}

static void rln_history_remove(rln_history_t *h, struct rln_history_line *line)
{
    rln_history_unedit(h, line);
    *(line->older ? &line->older->newer : &h->oldest) = line->newer;
    *(line->newer ? &line->newer->older : &h->newest) = line->older;
    if (h->entered == line)
        h->entered = NULL;
    h->free(line);
}

// Memory for a line of len characters under a header of size bytes, at the
// cost of the oldest lines when there is no other room. The line showing
// is not one of them: NULL when only it could go.
static void *rln_history_alloc(rln_history_t *h, size_t size, size_t len)
{
    for (;;)
    {
        void *mem = h->alloc(size + len + 1);
        if (mem || !h->oldest || h->oldest == h->at)
            return mem;
        rln_history_remove(h, h->oldest);
    }
}

// A read starts at the line being typed, with every line as it was entered
// and none entered yet.
static void rln_history_rewind(rln_history_t *h)
{
    h->at = NULL;
    h->entered = NULL;
    while (h->edits)
        rln_history_unedit(h, h->edits->of);
}

// The line showing is kept as it is now, for the read to come back to,
// unless it is as it was entered.
static void rln_history_keep(rln_history_t *h)
{
    rln_history_unedit(h, h->at);
    if (!strcmp(rln_buf, h->at ? h->at->text : ""))
        return;
    struct rln_history_edit *edit =
        rln_history_alloc(h, sizeof *edit, rln_buflen);
    if (!edit)
        return;
    edit->of = h->at;
    edit->next = h->edits;
    h->edits = edit;
    memcpy(edit->text, rln_buf, rln_buflen + 1u);
}

// dir > 0 walks toward older entries (up arrow), dir < 0 toward newer.
static void rln_history_step(int dir)
{
    rln_history_t *h = rln_history;
    if (!h || !(dir < 0 ? h->at : h->at ? h->at->older : h->newest))
        return;
    rln_buf[rln_buflen] = 0;
    rln_history_keep(h);
    // Keeping the line may have cost the oldest, so the step is found after.
    struct rln_history_line *to = dir < 0   ? h->at->newer
                                  : h->at ? h->at->older
                                          : h->newest;
    if (dir > 0 && !to)
        return;
    h->at = to;
    const char *text = to ? to->text : "";
    for (const struct rln_history_edit *edit = h->edits; edit; edit = edit->next)
        if (edit->of == to)
            text = edit->text;
    rln_buflen = (uint8_t)strlen(text);
    memcpy(rln_buf, text, rln_buflen + 1u);
    rln_action_taken = true;
    rln_fit();
    rln_bufpos = rln_buflen;
    rln_render_from(0);
}

// The line entered becomes the newest, unless it is empty or the newest
// again, and the oldest lines are dropped from a history that is full.
static void rln_history_add(void)
{
    rln_history_t *h = rln_history;
    if (!h)
        return;
    rln_history_rewind(h);
    if (!rln_buflen || (h->newest && !strcmp(h->newest->text, rln_buf)))
        return;
    struct rln_history_line *line =
        rln_history_alloc(h, sizeof *line, rln_buflen);
    if (!line)
        return;
    memcpy(line->text, rln_buf, rln_buflen + 1u);
    line->newer = NULL;
    line->older = h->newest;
    *(h->newest ? &h->newest->newer : &h->oldest) = line;
    h->newest = line;
    h->entered = line;
}

void rln_history_forget(rln_history_t *h)
{
    if (h->entered)
        rln_history_remove(h, h->entered);
}

// The cursor shape as DECSCUSR's Ps: 1 a block for overwrite, 5 a bar for
// insert, 0 the default. A terminal on the UART or telnet may print it, as
// minicom prints its 'q', so it goes to the screen alone and the mode shows
// nowhere else. Plain lines get none, and a terminal in VT52 mode takes no
// CSI.
static void rln_cursor_shape(uint8_t ps)
{
    if (!rln_prompt_col || vterm.vt52)
        return;
    char seq[] = "\33[0 q";
    seq[2] = (char)('0' + ps);
    rln_out_screen(seq, sizeof seq - 1);
}

// The read is over, or given up: nothing more is read for it, the callback
// never fires, its selection's highlight is off, a paste's line end it held
// is dropped, and the cursor has its default shape again. A completed line then gets its newline
// (rln_complete). Bracketed paste goes off last, so every terminal sees the
// line and its newline as typed, unbroken.
static void rln_read_end(bool completed)
{
    rln_unselect();
    rln_press_unit = RLN_UNIT_NONE;
    rln_callback = NULL;
    rln_idle_timeout_ms = 0;
    for (unsigned i = 0; i < RLN_SOURCE_COUNT; i++)
        if (rln_paste_in[i] >= RLN_PASTE_HELD)
            rln_paste_in[i] = RLN_PASTE_NONE;
    tio_read_end();
    rln_cursor_shape(0);
    if (completed && !rln_suppress_newline && !rln_below_line())
        rln_out_c('\n');
    if (rln_bracketing)
    {
        rln_bracketing = false;
        rln_out_s("\33[?2004l");
        typed_paste_unbracket();
    }
    rln_out_send(false);
}

void rln_read_cancel(void)
{
    rln_read_end(false);
}

/* The line is done at Enter: nothing is read after it, so whatever follows
 * is the next reader's. The newline takes the cursor to column 1 of the row
 * below the line, unless the line's own row break already did. SUPPRESS_NL
 * drops it so last-line field input keeps the cursor on its row. */
static void rln_complete(bool timed_out)
{
    rln_read_callback_t cc = rln_callback;
    rln_read_end(true);
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

/* Words are what a space or a path's separators part, so a file's name is
 * one word, extension and all: the rule the keys that move and kill by
 * words and the mouse's double click go by. With Alt and Ctrl together
 * those keys go by whole arguments, which only a space parts, so a path is
 * one. */
static bool rln_is_word_delimiter(char ch, bool arg)
{
    return ch == ' ' || (!arg && (ch == ':' || ch == '/' || ch == '\\'));
}

// Scan from rln_bufpos to the next boundary of a word, or with arg of an
// argument, in the given direction. dir > 0: end of current/next word;
// dir < 0: start of current/previous word. Returns the new buffer index
// (clamped to [0, rln_buflen]); equals rln_bufpos when already at the
// boundary.
static uint8_t rln_scan_word(int dir, bool arg)
{
    uint8_t pos = rln_bufpos;
    if (dir > 0)
    {
        while (pos < rln_buflen)
        {
            pos++;
            if (pos >= rln_buflen)
                break;
            if (rln_is_word_delimiter(rln_buf[pos], arg) &&
                !rln_is_word_delimiter(rln_buf[pos - 1], arg))
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
            if (!rln_is_word_delimiter(rln_buf[pos], arg) &&
                rln_is_word_delimiter(rln_buf[pos - 1], arg))
                break;
        }
    }
    return pos;
}

static void rln_line_word(int dir, bool arg)
{
    uint8_t to = rln_scan_word(dir, arg);
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

/* A CSI key's modifiers, as xterm sends them in its second parameter, less
 * one: Shift is 1, Alt 2 and Ctrl 4, and 0 is none. A key does something
 * only with the modifiers that mean something on it, and nothing with any
 * other: Shift selects with Left, Right, Home and End (rln_csi_selects),
 * Alt or Ctrl makes Left and Right move by words, and Ctrl makes Delete
 * kill a word. */
#define RLN_MOD_SHIFT 1u
#define RLN_MOD_ALT 2u
#define RLN_MOD_CTRL 4u
static unsigned rln_csi_mods(const tio_csi_t *csi)
{
    return csi->count > 1 && csi->param[1] > 1 ? csi->param[1] - 1u : 0;
}

/* xterm's modifyOtherKeys has a terminal send a key that types a byte as
 * a sequence, ESC [ 27 ; mod ; code ~ or ESC [ code ; mod u, at the level a
 * program set. This is the key again as it is sent without that: the
 * character, which Shift has already made, the control byte Ctrl makes of
 * it, and ESC in front for Alt, though not in front of a character past
 * ASCII, which a terminal's ESC before it is no key with either. So the
 * keys are the same keys at every level. Returns its length, 0 for any
 * other CSI. */
static size_t rln_other_key(const tio_csi_t *csi, char key[2])
{
    uint16_t code;
    if (csi->final == 'u')
        code = csi->param[0];
    else if (csi->final == '~' && csi->count == 3 && csi->param[0] == 27)
        code = csi->param[2];
    else
        return 0;
    const unsigned mods = rln_csi_mods(csi);
    if (code > UINT8_MAX || mods > (RLN_MOD_SHIFT | RLN_MOD_ALT | RLN_MOD_CTRL))
        return 0;
    char *p = key;
    if (mods & RLN_MOD_ALT && code < 0x80)
        *p++ = '\33';
    *p++ = mods & RLN_MOD_CTRL ? vtkey_ctrl((char)code) : (char)code;
    return (size_t)(p - key);
}

/* Left and Right, dir -1 and 1: by characters, or by words with Alt or
 * Ctrl. */
static void rln_line_arrow(const tio_csi_t *csi, int dir)
{
    const unsigned held = rln_csi_mods(csi) & ~RLN_MOD_SHIFT;
    if (!held)
        rln_step(dir * (int)rln_param_or_1(csi->param[0]));
    else if (held == RLN_MOD_ALT || held == RLN_MOD_CTRL)
        rln_line_word(dir, false);
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

/* Delete, and Backspace, delete the selection when there is one. */
static bool rln_delete_selection(void)
{
    if (rln_edit_from >= rln_edit_to)
        return false;
    rln_delete_range(rln_edit_from, rln_edit_to);
    return true;
}

static void rln_line_delete(void)
{
    if (!rln_delete_selection())
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
    if (rln_delete_selection())
        return;
    if (rln_bufpos)
        rln_delete_range(rln_bufpos - 1, rln_bufpos);
}

/* A kill key takes rln_buf[start..end) out of the line and into the read's
 * kill buffer, for Ctrl-Y to put back. Kills one after another are one
 * piece of the line again: one that reached back from the cursor goes
 * before what the buffer holds, and one that reached on goes after it. They
 * are all of the one line, which only they have changed, so together they
 * are no longer than a line. A kill with nothing to take leaves the buffer,
 * and the run it may be in, as they were. */
static void rln_kill_range(uint8_t start, uint8_t end, bool back)
{
    if (start >= end)
    {
        rln_killed = rln_kill_joins;
        return;
    }
    rln_killed = true;
    if (rln_kill)
    {
        const uint8_t count = (uint8_t)(end - start);
        if (!rln_kill_joins)
            rln_kill->len = 0;
        if (back)
            memmove(rln_kill->text + count, rln_kill->text, rln_kill->len);
        memcpy(rln_kill->text + (back ? 0 : rln_kill->len), rln_buf + start, count);
        rln_kill->len = (uint8_t)(rln_kill->len + count);
    }
    rln_delete_range(start, end);
}

/* The word before the cursor, dir -1, or after it, dir 1, or with arg the
 * argument, or the selection when there is one, which is then the word. */
static void rln_line_kill_word(int dir, bool arg)
{
    if (rln_edit_from < rln_edit_to)
        rln_kill_range(rln_edit_from, rln_edit_to, dir < 0);
    else if (dir < 0)
        rln_kill_range(rln_scan_word(-1, arg), rln_bufpos, true);
    else
        rln_kill_range(rln_bufpos, rln_scan_word(1, arg), false);
}

static void rln_line_kill_to_end(void)
{
    rln_kill_range(rln_bufpos, rln_buflen, false);
}

static void rln_line_kill_to_start(void)
{
    rln_kill_range(0, rln_bufpos, true);
}

/* rln_buf[from..to) becomes the count characters at text, as many of them
 * as the line has room for, and the cursor goes after them. A line longer
 * than the most it may be now, as one is when its program shortens that
 * while it is read, has room for none. */
static void rln_replace(uint8_t from, uint8_t to, const char *text, uint8_t count)
{
    const uint8_t gone = (uint8_t)(to - from);
    const uint8_t kept = (uint8_t)(rln_buflen - gone);
    const uint8_t cap = rln_effective_max();
    const uint8_t room = cap > kept ? (uint8_t)(cap - kept) : 0;
    if (count > room)
        count = room;
    if (!count && !gone)
        return;
    memmove(rln_buf + from + count, rln_buf + to, (size_t)(rln_buflen - to));
    memcpy(rln_buf + from, text, count);
    rln_buflen = (uint8_t)(rln_buflen - gone + count);
    rln_bufpos = (uint8_t)(from + count);
    rln_action_taken = true;
    rln_emit_tail_rewrite(from, gone > count ? (uint8_t)(gone - count) : 0);
}

/* Ctrl-Y puts back what the kill keys last took: it is inserted at the
 * cursor, or in the selection's place, whatever the mode, as much of it as
 * the line has room for. */
static void rln_line_yank(void)
{
    if (!rln_kill || !rln_kill->len)
        return;
    if (rln_edit_from < rln_edit_to)
        rln_replace(rln_edit_from, rln_edit_to, rln_kill->text, rln_kill->len);
    else
        rln_replace(rln_bufpos, rln_bufpos, rln_kill->text, rln_kill->len);
}

/* Ctrl-T swaps the character before the cursor with the one at it and steps
 * past them, and at the line's end swaps the last two. */
static void rln_line_transpose(void)
{
    if (!rln_bufpos || rln_buflen < 2)
        return;
    const uint8_t at = rln_bufpos < rln_buflen ? rln_bufpos : (uint8_t)(rln_buflen - 1);
    const char ch = rln_buf[at];
    rln_buf[at] = rln_buf[at - 1];
    rln_buf[at - 1] = ch;
    rln_bufpos = (uint8_t)(at + 1);
    rln_action_taken = true;
    rln_emit_tail_rewrite((uint8_t)(at - 1), 0);
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
    // A character replaces the selection, whatever the mode.
    if (rln_edit_from < rln_edit_to)
    {
        rln_replace(rln_edit_from, rln_edit_to, &ch, 1);
        return;
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
    rln_replace(rln_bufpos, rln_bufpos, &ch, 1);
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
    else if (ch == 20) // ctrl-t
        rln_line_transpose();
    else if (ch == 21) // ctrl-u
        rln_line_kill_to_start();
    else if (ch == 23) // ctrl-w
        rln_line_kill_word(-1, false);
    else if (ch == 25) // ctrl-y
        rln_line_yank();
    else
        rln_line_insert(ch);
}

static void rln_dispatch_Fe(uint8_t ch)
{
    /* Alt with a key is ESC and what the key types: Alt-B and Alt-F move
     * by words, and Alt-D and Alt-Backspace kill one. With Shift held or
     * Caps Lock on the letters come in upper case, and are the same keys.
     * With Ctrl held too they come as Ctrl-B, Ctrl-F and Ctrl-D, and
     * Backspace as the byte Ctrl makes of it, and they go by whole
     * arguments, as bash has them. While the console's terminal is in VT52
     * mode the keyboard sends a VT52's keys, and ESC A to D, H and F are
     * its arrows, Home and End. */
    if (vterm.vt52)
        switch (ch)
        {
        case 'A':
            rln_history_step(1);
            return;
        case 'B':
            rln_history_step(-1);
            return;
        case 'C':
            rln_step(1);
            return;
        case 'D':
            rln_step(-1);
            return;
        case 'F':
            rln_line_end();
            return;
        case 'H':
            rln_line_home();
            return;
        }
    const bool arg = ch == 2 || ch == 4 || ch == 6 || ch == (vterm.backarrow_bs ? 127 : '\b');
    if (ch == 'b' || ch == 'B' || ch == 2)
        rln_line_word(-1, arg);
    else if (ch == 'f' || ch == 'F' || ch == 6)
        rln_line_word(1, arg);
    else if (ch == 'd' || ch == 'D' || ch == 4)
        rln_line_kill_word(1, arg);
    else if (ch == 127 || ch == '\b')
        rln_line_kill_word(-1, arg);
}

// SS3 keys, which a terminal sends in application cursor mode: the arrows,
// Home and End. The keypad's in application keypad mode are the program's
// that asked for them, to read as the last key, and do nothing here, as a
// key with parameters, such as ESC O 5 P, does nothing.
static void rln_dispatch_SS3(const char *item, size_t len)
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
}

/* DCH, ICH and ECH are editing commands a program pokes. Typed, they are
 * function keys, Shift-F1 being CSI 1;2P, and do nothing. Home and End
 * come as CSI H and CSI F, or numbered: 1 and 4 from a VT220's Find and
 * Select, 7 and 8 from rxvt and the Linux console, rxvt's shifted ones
 * ending on '$' where the others end on '~'. Insert is 2 and Delete 3. A
 * key with a modifier that means nothing on it does nothing. */
static void rln_dispatch_CSI(com_source_t src, const tio_csi_t *csi)
{
    const uint8_t ch = csi->final;
    const unsigned mods = rln_csi_mods(csi);
    const uint16_t key = ch == '~' || ch == '$' ? csi->param[0] : 0;
    if (src == RLN_POKE && ch == 'P')
        rln_line_delete_n(csi);
    else if (src == RLN_POKE && ch == '@')
        rln_line_insert_n(csi);
    else if (src == RLN_POKE && ch == 'X')
        rln_line_erase_n(csi);
    else if (ch == 'C')
        rln_line_arrow(csi, 1);
    else if (ch == 'D')
        rln_line_arrow(csi, -1);
    else if (ch == '~' && key == 3 && mods == RLN_MOD_CTRL)
        rln_line_kill_word(1, false);
    else if (mods & ~RLN_MOD_SHIFT)
        return;
    else if (ch == 'F' || key == 4 || key == 8)
        rln_line_end();
    else if (ch == 'H' || key == 1 || key == 7)
        rln_line_home();
    else if (mods || ch == '$')
        return;
    else if (ch == 'A')
        rln_history_step(1);
    else if (ch == 'B')
        rln_history_step(-1);
    else if (key == 2)
        rln_toggle_overwrite();
    else if (key == 3)
        rln_line_delete();
}

// Saves the key just dispatched for RLN_LASTKEY.
static void rln_publish_lastkey(const char *item, size_t len)
{
    memcpy(rln_lastkey_buf, item, len);
    rln_lastkey_len = (uint8_t)len;
    rln_lastkey_action = rln_action_taken;
}

// Shift with Left, Right, Home or End, as xterm sends them, and rxvt, whose
// shifted Home and End end on '$': the keys that move with Shift held
// (rln_dispatch_CSI), Left and Right by words too.
static bool rln_csi_selects(const tio_csi_t *csi)
{
    const uint8_t ch = csi->final;
    const unsigned mods = rln_csi_mods(csi);
    const unsigned held = mods & ~RLN_MOD_SHIFT;
    if (ch == 'C' || ch == 'D')
        return mods & RLN_MOD_SHIFT && (!held || held == RLN_MOD_ALT || held == RLN_MOD_CTRL);
    if (ch == 'H' || ch == 'F')
        return mods == RLN_MOD_SHIFT;
    const uint16_t key = csi->param[0];
    if (key != 1 && key != 4 && key != 7 && key != 8)
        return false;
    return ch == '$' ? !mods : ch == '~' && mods == RLN_MOD_SHIFT;
}

// A typed item, dispatched while editing. A poked item is dispatched in
// overwrite mode, and a shape it changes is put back.
//
// A typed key that selects moves the cursor, and the selection goes from
// where the cursor was to where it is, or on from the anchor of the
// selection whose end the cursor was at. Any other key ends the selection,
// its highlight going first, before anything is drawn, and an edit it
// makes acts on the selection as the key found it.
static void rln_dispatch(com_source_t src, const char *item, size_t len,
                         const tio_csi_t *csi)
{
    /* ESC and one more byte is Alt with a key, whatever the byte: Alt-[
     * and Alt-O, which a modified-key sequence stands for, open nothing. */
    const bool seq = len > 2;
    const bool selects = src != RLN_POKE && seq && item[1] == '[' && rln_csi_selects(csi);
    const uint8_t from = rln_sel_anchor != rln_sel_end && rln_sel_end == rln_bufpos
                             ? rln_sel_anchor
                             : rln_bufpos;
    if (!selects)
    {
        rln_edit_from = rln_sel_lo();
        rln_edit_to = rln_sel_hi();
        rln_select(0, 0);
    }
    bool was_overwrite = rln_overwrite;
    if (src == RLN_POKE)
        rln_overwrite = true;
    rln_action_taken = false;
    rln_line_src = src;
    rln_kill_joins = rln_killed;
    rln_killed = false;
    if (len == 1)
        rln_dispatch_C0(src, (uint8_t)item[0]);
    else if (seq && item[1] == '[')
        rln_dispatch_CSI(src, csi);
    else if (seq && item[1] == 'O')
        rln_dispatch_SS3(item, len);
    else
        rln_dispatch_Fe((uint8_t)item[1]);
    rln_edit_from = rln_edit_to = 0;
    if (selects && rln_callback)
        rln_select(from, rln_bufpos);
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
//
// A bracketed paste, ESC [ 200 ~ to ESC [ 201 ~, is typing, and each line
// end in it is Enter: the read ends, and the next read goes on with the rest
// of the paste. One paste is not: a single line with its line end, a pair
// as one, and nothing after. That line end is dropped, so the line waits to
// be looked over, and Enter is for the person to press. So the first line
// end of a paste is held until its source shows what follows it. The
// brackets are dropped too.

/* src's paste holds its line end no longer. With its pair whole, the line
 * end that comes next is no half of it. */
static void rln_paste_let_go(com_source_t src)
{
    if (rln_paste_in[src] == RLN_PASTE_PAIRED)
        rln_line_ends[src] = 0;
    rln_paste_in[src] = RLN_PASTE_NONE;
}

/* The line end src's paste holds is Enter after all, which remembers it
 * for a half still to come (rln_enter). */
static void rln_paste_enter(com_source_t src)
{
    const char end = (char)rln_line_ends[src];
    const bool paired = rln_paste_in[src] == RLN_PASTE_PAIRED;
    const tio_csi_t none = {0};
    rln_paste_in[src] = RLN_PASTE_NONE;
    rln_dispatch(src, &end, 1, &none);
    rln_publish_lastkey(&end, 1);
    if (paired)
        rln_line_ends[src] = 0;
}

/* What follows the line end src's paste holds, looked at where it waits
 * and not taken. The closing bracket, past the other half of a pair: the
 * line end is dropped. Anything else: it is Enter. Nothing yet: it is held
 * on, and dropped when its source has sent no more in time, so a line
 * never enters itself behind a terminal that went quiet. True when it was
 * Enter. */
static bool rln_paste_decide(com_source_t src)
{
    static const char closing[] = "\33[201~";
    size_t at = 0;
    int c = com_peek(src, 0);
    if (rln_paste_in[src] == RLN_PASTE_HELD && (c == '\r' || c == '\n') &&
        c != rln_line_ends[src])
        c = com_peek(src, ++at);
    for (size_t i = 0; c == closing[i]; c = com_peek(src, at + ++i))
        if (i == sizeof closing - 2)
        {
            rln_paste_let_go(src);
            return false;
        }
    if (c >= 0)
    {
        rln_paste_enter(src);
        return true;
    }
    if (timer_mach_passed(rln_paste_deadline))
        rln_paste_let_go(src);
    return false;
}

static void rln_item(com_source_t src, const char *item, size_t len)
{
    tio_csi_t csi;
    bool is_csi = tio_csi_parse(item, len, &csi);
    if ((is_csi && (csi.priv || csi.inter)) ||
        (len > 1 && item[1] == '[' && !is_csi) ||
        (len == 3 && item[1] == 'N'))
        return;
    /* Past a line end held, the other half of its pair gets here when
     * what follows it has not come yet: it is taken, once, and the hold
     * goes on. Else only an Escape does: it is no closing bracket, and the
     * read the Enter ends has no use for it. */
    if (rln_paste_in[src] >= RLN_PASTE_HELD)
    {
        if (rln_paste_in[src] == RLN_PASTE_HELD && len == 1 &&
            (item[0] == '\r' || item[0] == '\n') && item[0] != rln_line_ends[src])
        {
            rln_paste_in[src] = RLN_PASTE_PAIRED;
            rln_paste_deadline = timer_mach_in_ms(RLN_PASTE_HOLD_MS);
        }
        else
            rln_paste_enter(src);
        return;
    }
    uint8_t was = rln_line_ends[src];
    rln_line_ends[src] = 0;
    if (is_csi && csi.final == '~' && csi.count == 1 &&
        (csi.param[0] == 200 || csi.param[0] == 201))
    {
        rln_paste_in[src] = csi.param[0] == 200 && src != RLN_POKE ? RLN_PASTE_FIRST
                                                                   : RLN_PASTE_NONE;
        return;
    }
    /* A modified-key sequence is the key it stands for, and the last key
     * is the sequence, as its terminal sent it. */
    char other[2];
    const size_t other_len = is_csi ? rln_other_key(&csi, other) : 0;
    const char *key = other_len ? other : item;
    const size_t key_len = other_len ? other_len : len;
    const bool line_end = key_len == 1 && (key[0] == '\r' || key[0] == '\n');
    if (line_end && was && key[0] != was)
        return;
    if (line_end && rln_paste_in[src] == RLN_PASTE_FIRST)
    {
        rln_paste_in[src] = RLN_PASTE_HELD;
        rln_line_ends[src] = (uint8_t)key[0];
        rln_paste_deadline = timer_mach_in_ms(RLN_PASTE_HOLD_MS);
        rln_paste_decide(src);
        return;
    }
    rln_dispatch(src, key, key_len, &csi);
    rln_publish_lastkey(item, len);
}

/* ----- Following the terminal ----- */

// The line starts where the console's terminal has its cursor, and is laid
// out at that row's columns, half the width on a double-size row, and those
// between the left and right margins for a cursor between them. Everything
// already printed reaches the terminals and vterm first, then the modes
// go to every terminal. A cursor in a pending wrap gets a CR LF, so the line
// starts in column 1 of the next row. A terminal in VT52 mode has no insert
// mode to turn off, and is sent a VT52's form of every move and erase.
static void rln_place(void);

static void rln_lay_out(void)
{
    manifold_flush();
    if (term_pos(rln_term())->wrap_pending)
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
    if (term_pos(rln_term())->wrap_pending)
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
    if (!plain && !vterm.vt52)
    {
        rln_out_s(RLN_MODES "\33[?2004h");
        rln_bracketing = true;
    }
    /* The line has the columns the terminal gives the cursor: from where a
     * CR lands to where a row of text ends, which are the left and right
     * margins for a cursor between them. Its columns are counted from the
     * first of those. It has no more rows than the scroll region the
     * cursor is in, where its row breaks scroll. */
    const term_grid_t *g = rln_term();
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
    /* A line end scrolls the region at its bottom margin, from a column
     * between the left and right margins, where the CR lands, and the
     * line's rows go up with it when the line starts inside the region. */
    rln_row = p->y;
    rln_left = (uint8_t)first;
    rln_ris_count = vterm.ris_count;
    rln_screen = rln_has_grid() ? RLN_NO_ROW : (uint8_t)(g->shown - g->screen);
    rln_bot = p->y >= g->margin_top && p->y <= g->margin_bot &&
                      first >= g->margin_left && first <= g->margin_right
                  ? g->margin_bot
                  : RLN_NO_ROW;
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
    rln_unselect();
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

/* ----- The mouse, and the copy and cut keys -----
 *
 * None of them has bytes, so each waits in the keyboard's queue as an
 * entry (core/hid/typed.h), and the line editor takes it in its place
 * among the keys: after those typed before it and before those typed
 * after. The mouse's entries are queued only while a line is read, and
 * those still waiting are taken back when the line's selection is ended
 * for it (rln_unselect), as it is when the read ends. */

void rln_mouse_press(uint8_t index, bool after, uint8_t clicks)
{
    typed_put(TYPED_ENTRY(TYPED_PRESS, index | (after ? RLN_PRESS_AFTER : 0) | (clicks & 3) << 9));
}

void rln_mouse_drag(uint8_t hy, uint8_t hx, uint8_t y, uint8_t x)
{
    if (rln_on_screen())
        typed_drag((uint8_t)rln_index_of(hy, hx, true), (uint8_t)rln_index_of(y, x, true));
}

void rln_mouse_release(void)
{
    if (rln_callback)
        typed_put(TYPED_ENTRY(TYPED_RELEASE, 0));
}

/* The word index i is in: a run of characters that are not word
 * delimiters, or the delimiter at i alone, and nothing at the line's end. */
static void rln_word_at(uint8_t i, uint8_t *lo, uint8_t *hi)
{
    *lo = *hi = i;
    if (i >= rln_buflen)
        return;
    *hi = (uint8_t)(i + 1);
    if (rln_is_word_delimiter(rln_buf[i], false))
        return;
    while (*lo && !rln_is_word_delimiter(rln_buf[*lo - 1], false))
        --*lo;
    while (*hi < rln_buflen && !rln_is_word_delimiter(rln_buf[*hi], false))
        ++*hi;
}

/* The mouse's press and drag become the selection, on the line as the
 * keys before them left it: by characters from the press to the drag, by
 * words out to the ends of the words under each, or the whole line. The
 * cursor follows the mouse to the selection's end, so a drag carries it
 * along, and what is typed next goes where the cursor shows. A click
 * selects nothing and puts the cursor where the mode then has it: the
 * block on the character under the pointer, and the bar at the gap nearest
 * the pointer, as IntelliJ places its carets. With up, the button has come
 * up, and what it selected fills the primary, as a selection the mouse
 * makes anywhere on the screen does. It is taken from the line the
 * selection is of, the line editor's own bytes. */
static void rln_mouse_apply(bool up)
{
    uint8_t anchor = rln_press_at < rln_buflen ? rln_press_at : rln_buflen;
    uint8_t end = rln_drag_at < rln_buflen ? rln_drag_at : rln_buflen;
    if (rln_press_unit == RLN_UNIT_CLICK)
        anchor = end = rln_overwrite ? anchor : end;
    else if (rln_press_unit == RLN_UNIT_WORD)
    {
        uint8_t alo, ahi, elo, ehi;
        rln_word_at(anchor, &alo, &ahi);
        rln_word_at(end, &elo, &ehi);
        const bool on = end >= anchor;
        anchor = on ? alo : ahi;
        end = on ? (ehi > ahi ? ehi : ahi) : (elo < alo ? elo : alo);
    }
    else if (rln_press_unit == RLN_UNIT_LINE)
    {
        anchor = 0;
        end = rln_buflen;
    }
    rln_select(anchor, end);
    if (up && anchor != end)
    {
        const uint8_t lo = rln_sel_lo();
        clip_copy_bytes(CLIP_PRIMARY, &rln_buf[lo], (size_t)(rln_sel_hi() - lo));
    }
    rln_bufpos = end;
    rln_sync_cursor_to(rln_bufpos);
}

bool HOST_IN_RAM_FUNC rln_reading(void)
{
    return rln_callback != NULL;
}

bool rln_idle(void)
{
    return !typed_entry() && tio_idle();
}

bool rln_selected(void)
{
    return rln_callback && rln_sel_anchor != rln_sel_end;
}

/* An entry taken from the keyboard's queue. The copy key puts the
 * selection in the clipboard and leaves it selected, and a cut key puts it
 * there and deletes it; with nothing selected neither does anything. The
 * mouse's press starts a click, or a selection by words or lines; its drag
 * moves the two ends, a click becoming a selection by characters; and its
 * release fills the primary. A drag or a release whose press was not taken
 * in this read's line does nothing, and a click's release has nothing to
 * fill it with. Any of them ends a run of kills, as a key between two
 * kills does, and none is the last key. */
static void rln_entry(typed_entry_t e)
{
    const uint8_t at = (uint8_t)e;
    bool up = false;
    rln_killed = false;
    switch (TYPED_KIND(e))
    {
    case TYPED_COPY:
    case TYPED_CUT:
    {
        const uint8_t lo = rln_sel_lo(), hi = rln_sel_hi();
        if (lo == hi)
            return;
        clip_copy_bytes(CLIP_CLIPBOARD, &rln_buf[lo], (size_t)(hi - lo));
        if (TYPED_KIND(e) == TYPED_CUT)
        {
            rln_select(0, 0);
            rln_delete_range(lo, hi);
        }
        return;
    }
    case TYPED_PRESS:
        rln_press_at = rln_drag_at = at;
        rln_press_unit = RLN_PRESS_CLICKS(e) == 3   ? RLN_UNIT_LINE
                         : RLN_PRESS_CLICKS(e) == 2 ? RLN_UNIT_WORD
                                                    : RLN_UNIT_CLICK;
        if (rln_press_unit == RLN_UNIT_CLICK && (e & RLN_PRESS_AFTER) && at < rln_buflen)
            rln_drag_at++;
        break;
    case TYPED_HELD:
        rln_press_at = at;
        return;
    case TYPED_DRAG:
        if (rln_press_unit == RLN_UNIT_NONE)
            return;
        if (rln_press_unit == RLN_UNIT_CLICK)
            rln_press_unit = RLN_UNIT_CHAR;
        rln_drag_at = at;
        break;
    default:
        if (rln_press_unit == RLN_UNIT_NONE || rln_press_unit == RLN_UNIT_CLICK)
            return;
        up = true;
    }
    rln_mouse_apply(up);
}

/* A paste's line end that is held is looked at again first, and is Enter
 * here when more of its paste has come. Then the entry of the keyboard's
 * queue that is the line editor's is taken in its turn: when it is at the
 * queue's front, and no byte before it waits in the RIA's RX latch, which
 * takes the keyboard's next byte. True when either was taken. From flash, as the keys' edits are, so the task in RAM
 * does not take it in. */
static bool HOST_IN_FLASH_FUNC rln_take_entry(void)
{
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < RLN_POKE; s++)
        if (rln_paste_in[s] >= RLN_PASTE_HELD && rln_paste_decide(s))
            return true;
    typed_entry_t e;
    if (com_rx_peek(COM_SOURCE_KEYBOARD) >= 0 || !typed_take(&e))
        return false;
    rln_entry(e);
    return true;
}

/* ----- Top-level task / driver hooks ----- */

static void rln_start(rln_read_callback_t callback, rln_history_t *history, rln_kill_t *kill)
{
    /* The reply to the last read's last key goes out first. A read started
     * before that read was handed its line replaces it, and the line is
     * dropped: its readers wait for rln_replying to end before they ask for
     * another. */
    rln_out_then = NULL;
    rln_unselect();
    rln_press_unit = RLN_UNIT_NONE;
    rln_out_send(true);
    rln_idle_timeout_ms = 0;
    rln_buflen = 0;
    rln_bufpos = 0;
    rln_callback = callback;
    rln_history = history;
    if (history)
        rln_history_rewind(history);
    rln_kill = kill;
    rln_killed = false;
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

void rln_read_line(rln_read_callback_t callback, rln_history_t *history, rln_kill_t *kill)
{
    rln_start(callback, history, kill);
    rln_laying_out = !rln_lay_out_now();
    rln_out_send(false);
}

void rln_read_line_script(rln_read_callback_t callback, uint32_t timeout_ms)
{
    assert(timeout_ms);
    rln_start(callback, NULL, NULL);
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

/* The items waiting are taken, each once the reply to the one before it
 * has gone out, and true once all are taken and answered. What the
 * keyboard's queue holds for the line editor alone is taken among them, in
 * its turn. */
static bool HOST_IN_RAM_FUNC rln_take_items(void)
{
    if (!rln_out_send(false) || !rln_callback)
        return false;
    if (rln_laying_out && (rln_laying_out = !rln_lay_out_now()))
        return false;
    char item[TIO_ITEM_MAX];
    com_source_t src;
    size_t len;
    for (;;)
    {
        if (rln_callback)
        {
            rln_follow();
            rln_selection_follow();
        }
        if (!rln_callback)
            return true;
        if (!rln_take_entry())
        {
            if (!(len = tio_read(&src, item)))
                return true;
            rln_idle_deadline = timer_mach_in_ms(rln_idle_timeout_ms);
            rln_item(src, item, len);
        }
        if (!rln_out_send(false))
            return false;
    }
}

void HOST_IN_RAM_FUNC rln_task(void)
{
    const bool answered = rln_take_items();
    rln_selection_follow();
    if (rln_hl_due && !rln_out_len && manifold_drained())
        rln_show();
    if (answered && rln_idle_timeout_ms && timer_mach_passed(rln_idle_deadline))
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
    rln_sel_anchor = rln_sel_end = 0;
    rln_hl_due = false;
    rln_press_unit = RLN_UNIT_NONE;
    rln_bracketing = false;
    term_grid_init(&rln_grid, 80, rln_grid_lines, NULL, RLN_GRID_ROWS);
}

void rln_run(void)
{
    rln_max_length = 254; // reserve 1 for the stdin newline
}

/* A read cut short by a stop or a break leaves the cursor on a new line
 * past everything typed, with its default shape and no highlight. A
 * program's own size goes back to the screen's, and every source's paste
 * is over, a line end it held dropped with it (rln_reset). */
void rln_stop(void)
{
    if (rln_callback)
    {
        rln_unselect();
        rln_sync_past_line();
        if (!rln_below_line())
            rln_out_s("\r\n");
        rln_read_cancel();
    }
    rln_out_then = NULL;
    rln_out_send(true);
    rln_reset();
}

/* No read, no selection and no bracketed paste of any source, and the
 * third grid off the screen's terminal, with nothing drawn: after a failed
 * load the line's indexes and geometry may hold anything, and the console
 * they would draw through is not reset yet. DRAW's RESET runs first, so the
 * grid list is sound to search. The rest is set again by the next read's
 * start. */
void rln_reset(void)
{
    if (rln_has_grid())
        term_drop_grid(&vterm, &rln_grid);
    rln_out_len = rln_out_lit_len = rln_out_at = 0;
    rln_out_sent = 0;
    rln_out_then = NULL;
    memset(rln_paste_in, 0, sizeof rln_paste_in);
    rln_init();
}

void rln_set_plain(bool plain)
{
    rln_plain = plain;
}

uint16_t rln_get_term_width(void)
{
    return rln_term()->width;
}

uint16_t rln_get_term_height(void)
{
    return rln_term()->height;
}

/* The third grid starts as a copy of the showing one and takes the pinned
 * size. An axis not pinned takes the showing grid's when the other is
 * pinned, and keeps it: the program's terminal is a fixed size until both
 * are 0 again, when it is dropped. The screen's grids are never touched. */
static void rln_size_grid(void)
{
    term_t *t = &vterm;
    if (!rln_pinned_width && !rln_pinned_height)
    {
        if (rln_has_grid())
            term_drop_grid(t, &rln_grid);
        return;
    }
    const term_grid_t *shown = vterm_get_shown(t);
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
    rln_paste_in[src] = RLN_PASTE_NONE;
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
     * now, so it goes out before the poke edits it. A poke ends the
     * selection. */
    rln_out_send(true);
    rln_unselect();
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
        tio_step_t step = tio_frame(&frame, ch, false);
        if (step == TIO_AGAIN)
        {
            len = 0;
            step = tio_frame(&frame, ch, false);
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
