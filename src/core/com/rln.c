/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/timer.h"
#include "machine.h"
#include "core/api/api.h"
#include "core/api/std.h"
#include "core/com/rln.h"
#include "core/com/com.h"
#include "core/com/tio.h"
#include "core/sys/driver.h"
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
**   - rln owns only rln_max_length cells from the start of input, so no EL
**     unless the input can take more than its row.
**   - A plain line (rln_set_plain, or a script read) lays nothing out: it
**     echoes, moves with CUF and CUB, and the terminal wraps it.
*/

#define RLN_BUF_SIZE 256
_Static_assert(RLN_BUF_SIZE == RLN_LINE_MAX, "rln.h publishes this bound");
#define RLN_HISTORY_SIZE 8
#define RLN_LASTKEY_MAX 32
_Static_assert(RLN_LASTKEY_MAX >= TIO_ITEM_MAX, "every key fits the last key");

// rln_caps modes — public ABI is uint8_t, so the values are wire values.
#define RLN_CAPS_OFF 0
#define RLN_CAPS_UPPER 1
#define RLN_CAPS_SWAP 2
// Terms < 29 cols may not reach 254/255 chars in 10 rows.
#define RLN_MAX_ROWS 10

// The 6502's poke stream is a source of its own, after the com sources.
#define RLN_POKE COM_SOURCE_ANY
#define RLN_SOURCE_COUNT (RLN_POKE + 1)


// History storage. The live edit buffer is rln_buf; history[0] is a
// scratch slot used to stash the user's unsubmitted line while they
// navigate older entries. history[1..RLN_HISTORY_SIZE-1] hold previous
// entries, newest at [1].
static char rln_history[RLN_HISTORY_SIZE][RLN_BUF_SIZE];
static uint8_t rln_history_count;
static uint8_t rln_history_pos;

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
static bool rln_skip_history;
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
static uint16_t rln_prompt_col;        // 1-based, 0 = no prompt column, no wrapping
static uint16_t rln_width;             // the columns of the prompt's row
static uint8_t rln_grid_width;         // the shown grid's width, watched for a change
static uint8_t rln_rows;               // the most rows the line may take
static uint8_t rln_cur_idx;            // buffer index whose screen position the cursor is at
static bool rln_overwrite;             // preserved across rln_read_line calls; cleared by rln_init (rln_stop)
static uint8_t rln_rendered_max_row;   // highest row index rln has written to in the current line
static uint8_t rln_last_render_buflen; // buflen as of the last render

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

void rln_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    for (int i = 0; i < RLN_HISTORY_SIZE; i++)
        sst_put(c, rln_history[i], RLN_BUF_SIZE);
    sst_put_u8(c, rln_history_count);
    sst_put_u8(c, rln_history_pos);
    sst_put(c, rln_buf, RLN_BUF_SIZE);
    /* A callback is a function pointer, which cannot be written to a blob, so
     * it is saved as a token. std.c's reader is the only one this file can
     * name, and any other callback fails the save rather than being restored
     * as the wrong reader. */
    if (!rln_callback)
        sst_put_u8(c, 0);
    else if (rln_callback == std_rln_reader())
        sst_put_u8(c, 1);
    else
        sst_fail(c);
    sst_put_u64(c, rln_idle_deadline);
    sst_put_u8(c, rln_buflen);
    sst_put_u8(c, rln_bufpos);
    sst_put_bool(c, rln_skip_history);
    sst_put_u8(c, rln_max_length);
    sst_put_u32(c, rln_idle_timeout_ms);
    sst_put_u8(c, rln_caps);
    sst_put_bool(c, rln_suppress_newline);
    sst_put_u16(c, rln_prompt_col);
    sst_put_u16(c, rln_width);
    sst_put_u8(c, rln_grid_width);
    sst_put_u8(c, rln_rows);
    sst_put_u8(c, rln_cur_idx);
    sst_put_bool(c, rln_overwrite);
    sst_put_u8(c, rln_rendered_max_row);
    sst_put_u8(c, rln_last_render_buflen);
    sst_put(c, rln_lastkey_buf, RLN_LASTKEY_MAX);
    sst_put_u8(c, rln_lastkey_len);
    sst_put_bool(c, rln_action_taken);
    sst_put_bool(c, rln_lastkey_action);
    sst_put(c, rln_line_ends, RLN_SOURCE_COUNT);
}

bool rln_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    static char history[RLN_HISTORY_SIZE][RLN_BUF_SIZE];
    for (int i = 0; i < RLN_HISTORY_SIZE; i++)
        sst_get(c, history[i], RLN_BUF_SIZE);
    uint8_t hcount = sst_get_u8(c), hpos = sst_get_u8(c);
    static char line[RLN_BUF_SIZE];
    sst_get(c, line, sizeof line);
    uint8_t reader = sst_get_u8(c);
    uint64_t idle = sst_get_u64(c);
    uint8_t buflen = sst_get_u8(c), bufpos = sst_get_u8(c);
    bool skip = sst_get_bool(c);
    uint8_t maxlen = sst_get_u8(c);
    uint32_t idle_ms = sst_get_u32(c);
    uint8_t caps = sst_get_u8(c);
    bool suppress = sst_get_bool(c);
    uint16_t pcol = sst_get_u16(c), width = sst_get_u16(c);
    uint8_t grid_width = sst_get_u8(c), rows = sst_get_u8(c);
    uint8_t cur = sst_get_u8(c);
    bool over = sst_get_bool(c);
    uint8_t maxrow = sst_get_u8(c), lastlen = sst_get_u8(c);
    static uint8_t lastkey[RLN_LASTKEY_MAX];
    sst_get(c, lastkey, sizeof lastkey);
    uint8_t lastkey_len = sst_get_u8(c);
    bool acted = sst_get_bool(c), key_acted = sst_get_bool(c);
    uint8_t line_end[RLN_SOURCE_COUNT];
    sst_get(c, line_end, sizeof line_end);
    /* The lengths need no range check because a uint8_t cannot reach past a
     * buffer this size, and the static assert keeps that true if RLN_BUF_SIZE
     * changes. The indexes are checked because they can exceed the lengths
     * they index. */
    _Static_assert(RLN_BUF_SIZE > UINT8_MAX, "a byte length must not escape rln_buf");
    if (!sst_ok(c) || reader > 1 || bufpos > buflen ||
        cur > buflen || lastkey_len > RLN_LASTKEY_MAX ||
        hcount > RLN_HISTORY_SIZE || hpos > RLN_HISTORY_SIZE)
        return false;

    memcpy(rln_history, history, sizeof rln_history);
    rln_history_count = hcount;
    rln_history_pos = hpos;
    memcpy(rln_buf, line, sizeof rln_buf);
    rln_callback = reader ? std_rln_reader() : NULL;
    rln_idle_deadline = idle;
    rln_buflen = buflen;
    rln_bufpos = bufpos;
    rln_skip_history = skip;
    rln_max_length = maxlen;
    rln_idle_timeout_ms = idle_ms;
    rln_caps = caps;
    rln_suppress_newline = suppress;
    rln_prompt_col = pcol;
    rln_width = width;
    rln_grid_width = grid_width;
    rln_rows = rows;
    rln_cur_idx = cur;
    rln_overwrite = over;
    rln_rendered_max_row = maxrow;
    rln_last_render_buflen = lastlen;
    memcpy(rln_lastkey_buf, lastkey, sizeof rln_lastkey_buf);
    rln_lastkey_len = lastkey_len;
    rln_action_taken = acted;
    rln_lastkey_action = key_acted;
    memcpy(rln_line_ends, line_end, sizeof rln_line_ends);
    return true;
}

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

// Move to column col of the cursor's row: CR, then CUF. VT100 and VT102 have
// no CHA. CR goes to the real cell from a pending wrap at the margin, and
// clears it.
static void rln_emit_col(uint16_t col)
{
    com_putchar('\r');
    if (col > 1)
        com_printf("\33[%uC", col - 1);
}

// Move the visible cursor so that it sits at the screen position
// corresponding to buffer index `target`.
static void rln_sync_cursor_to(uint8_t target)
{
    // Plain lines have no prompt column, so no row math: plain horizontal
    // cursor moves, and the terminal wraps.
    if (rln_prompt_col == 0)
    {
        int delta = (int)target - (int)rln_cur_idx;
        if (delta > 0)
            com_printf("\33[%dC", delta);
        else if (delta < 0)
            com_printf("\33[%dD", -delta);
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
            com_printf("\33[%uB", tr - fr);
        else if (tr < fr)
            com_printf("\33[%uA", fr - tr);
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
    com_putchar(ch);
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
        com_printf("\r\n");
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
    for (uint8_t i = 0; i < count; i++)
        com_putchar(' ');
    if (!rln_prompt_col)
        com_printf("\33[%uD", count);
}

// Walk down `rows` existing rows from the cursor, clearing each, then return up
// to the start row at column `col`.
static void rln_clear_rows_below(uint8_t rows, uint16_t col)
{
    for (uint8_t k = 0; k < rows; k++)
        com_printf("\33[B\r\33[K");
    if (rows)
    {
        com_printf("\33[%uA", rows);
        if (col > 1)
            com_printf("\33[%uC", col - 1);
    }
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
            com_putchar(rln_buf[i]);
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
        if (rln_cur_idx == rln_buflen && rln_last_render_buflen > rln_buflen)
            com_printf("\33[K");
        uint8_t er;
        uint16_t ec;
        rln_buf_to_screen(rln_cur_idx, &er, &ec);
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
        com_putchar(rln_buf[i]);
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
    if (rln_idle_timeout_ms || rln_skip_history)
        return;
    if (dir > 0 && rln_history_pos >= rln_history_count)
        return;
    if (dir < 0 && !rln_history_pos)
        return;
    rln_buf[rln_buflen] = 0;
    memcpy(rln_history[rln_history_pos], rln_buf, rln_buflen + 1);
    rln_history_pos += dir;
    memcpy(rln_buf, rln_history[rln_history_pos], RLN_BUF_SIZE);
    rln_action_taken = true;
    rln_replace_buf_from_history();
}

static void rln_history_add(void)
{
    if (rln_idle_timeout_ms || rln_skip_history)
        return;
    if (rln_buflen == 0)
        return;
    rln_buf[rln_buflen] = 0;
    if (rln_history_count > 0 && strcmp(rln_history[1], rln_buf) == 0)
        return;
    memmove(rln_history[2], rln_history[1], (size_t)(RLN_HISTORY_SIZE - 2) * RLN_BUF_SIZE);
    memcpy(rln_history[1], rln_buf, rln_buflen + 1);
    rln_history[0][0] = 0;
    if (rln_history_count < RLN_HISTORY_SIZE - 1)
        rln_history_count++;
}

/* The line is done at Enter: nothing is read after it, so whatever follows
 * is the next reader's. The newline takes the cursor to column 1 of the row
 * below the line, unless the line's own row break already did. SUPPRESS_NL
 * drops it so last-line field input keeps the cursor on its row. */
static void rln_complete(bool timed_out)
{
    rln_read_callback_t cc = rln_callback;
    rln_idle_timeout_ms = 0;
    rln_callback = NULL;
    tio_read_end();
    if (!rln_suppress_newline && !rln_below_line())
        com_putchar('\n');
    cc(timed_out, rln_buf);
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

// The cursor shape for the mode, a block for overwrite and a bar for insert.
// tio sends it only to the screen, so on the other terminals the mode has no
// visual indicator. Plain lines get none.
static void rln_emit_mode_cursor(void)
{
    if (rln_prompt_col)
        tio_cursor_shape(rln_overwrite ? 1 : 5);
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
    // older meta-prefix terminals send for the same binding.
    if (ch == 'b' || ch == 2)
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
// out at that row's columns.
static void rln_lay_out(void)
{
    tio_line_t l;
    tio_read_start(&l);
    rln_prompt_col = rln_plain ? 0 : l.col;
    rln_width = l.cols;
    rln_grid_width = l.width;
    rln_rows = l.height < RLN_MAX_ROWS ? l.height : RLN_MAX_ROWS;
    rln_emit_mode_cursor();
}

// The line is drawn again when the shown grid's width changes under it: a new
// canvas, a VGA reset or a program's own width. No terminal is trusted to have
// reflowed it, so it starts over on a clean row below the old line, at the
// new width, without the prompt, cut to what the new layout holds. A plain
// line is never drawn again.
static void rln_follow(void)
{
    if (!rln_prompt_col || tio_get_term_width() == rln_grid_width)
        return;
    rln_sync_past_line();
    com_clean_line();
    rln_lay_out();
    rln_fit();
    rln_cur_idx = 0;
    rln_rendered_max_row = 0;
    rln_last_render_buflen = 0;
    rln_render_from(0);
}

/* ----- Top-level task / driver hooks ----- */

static void rln_start(rln_read_callback_t callback)
{
    rln_idle_timeout_ms = 0;
    rln_skip_history = false;
    rln_buflen = 0;
    rln_bufpos = 0;
    rln_callback = callback;
    rln_history_pos = 0;
    rln_buf[0] = 0;
    rln_lastkey_len = 0;
    rln_action_taken = false;
    rln_lastkey_action = false;

    rln_prompt_col = 0;
    rln_cur_idx = 0;
    rln_rendered_max_row = 0;
    rln_last_render_buflen = 0;
    rln_line_src = COM_SOURCE_ANY;
}

void rln_read_line(rln_read_callback_t callback)
{
    rln_start(callback);
    rln_lay_out();
}

void rln_read_line_script(rln_read_callback_t callback, uint32_t timeout_ms)
{
    assert(timeout_ms);
    rln_start(callback);
    rln_idle_timeout_ms = timeout_ms;
    rln_idle_deadline = timer_mach_in_ms(rln_idle_timeout_ms);
}

void rln_read_line_no_history(rln_read_callback_t callback)
{
    rln_read_line(callback);
    rln_skip_history = true;
}

com_source_t rln_line_source(void)
{
    return rln_line_src;
}

void rln_read_cancel(void)
{
    rln_callback = NULL;
    rln_idle_timeout_ms = 0;
    tio_read_end();
}

bool rln_read_flush(void)
{
    if (!rln_callback || !rln_buflen)
        return false;
    rln_line_src = COM_SOURCE_ANY;
    rln_finish_line(false);
    return true;
}

void HOST_IN_RAM_FUNC rln_task(void)
{
    if (!rln_callback)
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
}

void rln_run(void)
{
    rln_max_length = 254; // reserve 1 for the stdin newline
}

/* A read cut short by a stop or a break leaves the cursor on a new line
 * past everything typed. */
void rln_stop(void)
{
    if (rln_callback)
    {
        rln_sync_past_line();
        if (!rln_below_line())
            com_printf("\r\n");
    }
    rln_init();
}

void rln_set_plain(bool plain)
{
    rln_plain = plain;
}

void HOST_IN_RAM_FUNC rln_forget_source(unsigned src)
{
    if (src < COM_SOURCE_COUNT)
        rln_line_ends[src] = 0;
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
    char item[TIO_ITEM_MAX];
    size_t len = 0;
    uint8_t frame = 0;
    // An empty string sends 0x00.
    const char *p = str;
    do
    {
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
