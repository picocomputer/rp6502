/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/timer.h"
#include "machine.h"
#include "core/api/api.h"
#include "core/api/std.h"
#include "core/str/rln.h"
#include "core/com/com.h"
#include "core/com/tio.h"
#include "core/sys/driver.h"
#include <string.h>
#include <ctype.h>
#include <assert.h>

/* Console manifold compatibility rules.
**
** rln drives a "manifold" of terminals: primary UART + optional telnet
** shadow + optional VGA. Design goals: (1) secondary terminals wider
** than the primary still render correctly; (2) callers can place rln
** inside a wider form by setting rln_max_length to reserve only the
** cells rln is allowed to own.
**
** Runtime invariants:
**   - No absolute Y cursor movement; no writes past rln_max_length.
**   - The wrapping input is one logical line: rln writes it continuously
**     and lets the terminal autowrap (the standard readline/linenoise
**     model), so the terminal does the wrapping and the resize reflow.
**     rln keeps its own row model and relies on pending-wrap (xenl) at
**     the margin.
**   - rln owns only rln_max_length cells from the start of input, so
**     no ICH/DCH unless rln owns the entire visible input region.
**   - Dynamic resize (NAWS / CPR refinement) trusts the terminal to rewrap
**     on a width change; non-reflowing terminals aren't supported for it.
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

// Display state. The line is laid out at the prompt column and the width tio
// had when it was last drawn.
static bool rln_editing;               // geometry is ready and the line is drawn
static uint16_t rln_prompt_col;        // 1-based, 0 = no prompt column, no wrapping
static uint16_t rln_width;
static uint8_t rln_cur_idx;            // buffer index whose screen position the cursor is at
static bool rln_overwrite;             // preserved across rln_read_line calls; cleared by rln_init (rln_stop)
static uint8_t rln_rendered_max_row;   // highest row index rln has written to in the current line
static uint8_t rln_last_render_buflen; // buflen as of the last render

// The CR or LF each source, the poke stream included, last ended a line
// with, so the other half of a CRLF pair arriving next is absorbed instead of
// ending a second line.
static uint8_t rln_line_ends[RLN_SOURCE_COUNT];

// The typed items that arrive before the edit starts, in the order they came,
// each as its source, its length, and its bytes.
static uint8_t rln_held[RLN_BUF_SIZE];
static uint16_t rln_held_len;

// Lastkey capture for the 6502 API. rln_action_taken is the live
// "this dispatch mutated state" flag, sampled at lastkey publish time.
// Typed and poked dispatches both populate these; the terminals' answers,
// which tio takes, never reach them.
static uint8_t rln_lastkey_buf[RLN_LASTKEY_MAX];
static uint8_t rln_lastkey_len;
static bool rln_action_taken;
static bool rln_lastkey_action;

/* Every held record names a source and fits, whole, inside the length. */
static bool rln_held_valid(const uint8_t *held, uint16_t len)
{
    if (len > RLN_BUF_SIZE)
        return false;
    for (uint16_t at = 0; at < len; at += 2 + held[at + 1])
        if (len - at < 2 || held[at] >= RLN_SOURCE_COUNT || !held[at + 1] ||
            held[at + 1] > TIO_ITEM_MAX || len - at - 2 < held[at + 1])
            return false;
    return true;
}

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
    sst_put_bool(c, rln_editing);
    sst_put_u16(c, rln_prompt_col);
    sst_put_u16(c, rln_width);
    sst_put_u8(c, rln_cur_idx);
    sst_put_bool(c, rln_overwrite);
    sst_put_u8(c, rln_rendered_max_row);
    sst_put_u8(c, rln_last_render_buflen);
    sst_put(c, rln_lastkey_buf, RLN_LASTKEY_MAX);
    sst_put_u8(c, rln_lastkey_len);
    sst_put_bool(c, rln_action_taken);
    sst_put_bool(c, rln_lastkey_action);
    sst_put(c, rln_line_ends, RLN_SOURCE_COUNT);
    sst_put(c, rln_held, RLN_BUF_SIZE);
    sst_put_u16(c, rln_held_len);
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
    bool editing = sst_get_bool(c);
    uint16_t pcol = sst_get_u16(c), width = sst_get_u16(c);
    uint8_t cur = sst_get_u8(c);
    bool over = sst_get_bool(c);
    uint8_t maxrow = sst_get_u8(c), lastlen = sst_get_u8(c);
    static uint8_t lastkey[RLN_LASTKEY_MAX];
    sst_get(c, lastkey, sizeof lastkey);
    uint8_t lastkey_len = sst_get_u8(c);
    bool acted = sst_get_bool(c), key_acted = sst_get_bool(c);
    uint8_t line_end[RLN_SOURCE_COUNT];
    sst_get(c, line_end, sizeof line_end);
    static uint8_t held[RLN_BUF_SIZE];
    sst_get(c, held, sizeof held);
    uint16_t held_len = sst_get_u16(c);
    /* The lengths need no range check because a uint8_t cannot reach past a
     * buffer this size, and the static assert keeps that true if RLN_BUF_SIZE
     * changes. The indexes are checked because they can exceed the lengths
     * they index. */
    _Static_assert(RLN_BUF_SIZE > UINT8_MAX, "a byte length must not escape rln_buf");
    if (!sst_ok(c) || reader > 1 || bufpos > buflen ||
        cur > buflen || lastkey_len > RLN_LASTKEY_MAX ||
        hcount > RLN_HISTORY_SIZE || hpos > RLN_HISTORY_SIZE ||
        !rln_held_valid(held, held_len))
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
    rln_editing = editing;
    rln_prompt_col = pcol;
    rln_width = width;
    rln_cur_idx = cur;
    rln_overwrite = over;
    rln_rendered_max_row = maxrow;
    rln_last_render_buflen = lastlen;
    memcpy(rln_lastkey_buf, lastkey, sizeof rln_lastkey_buf);
    rln_lastkey_len = lastkey_len;
    rln_action_taken = acted;
    rln_lastkey_action = key_acted;
    memcpy(rln_line_ends, line_end, sizeof rln_line_ends);
    memcpy(rln_held, held, sizeof rln_held);
    rln_held_len = held_len;
    return true;
}

/* The line is done at Enter: nothing is read after it, so whatever follows
 * is the next reader's. SUPPRESS_NL drops the newline so last-line field
 * input keeps the cursor on its row. */
static void rln_complete(bool timed_out)
{
    rln_read_callback_t cc = rln_callback;
    rln_idle_timeout_ms = 0;
    rln_callback = NULL;
    tio_read_end();
    if (!rln_suppress_newline)
        com_putchar('\n');
    cc(timed_out, rln_buf);
}

/* ----- Screen position math (multi-line mode) ----- */

// True when rln must not emit EL, ICH, or DCH. Two cases:
//   1. Geometry untrusted (prompt_col == 0). No row math possible, and
//      callers may be inside a form whose adjacent data we mustn't clobber.
//   2. Geometry known and the configured max_length fits on the prompt
//      row, so the input cannot wrap. Same form-data concern applies.
// Wrap mode (returns false) is only when prompt_col is known AND the
// input genuinely can wrap to a second row — the multi-row regime where
// per-row EL and ICH/DCH are appropriate and necessary.
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

// Emit relative cursor moves from (fr,fc) to (tr,tc). Downward moves use
// LF, not CUD: CUD doesn't scroll, so at the bottom of the screen it
// no-ops and renders clobber the same row. These are cursor moves, not
// content; an explicit move cancels any pending wrap, so \r and \n are
// unambiguous.
static void rln_emit_move(uint8_t fr, uint16_t fc, uint8_t tr, uint16_t tc)
{
    if (tr > fr)
    {
        for (uint8_t i = 0, n = (uint8_t)(tr - fr); i < n; i++)
            com_putchar('\n');
        com_printf("\r");
        if (tc > 1)
            com_printf("\33[%uC", tc - 1);
    }
    else if (tr < fr)
    {
        com_printf("\33[%uA\r", fr - tr);
        if (tc > 1)
            com_printf("\33[%uC", tc - 1);
    }
    else if (tc != fc)
    {
        if (tc > fc)
            com_printf("\33[%uC", tc - fc);
        else
            com_printf("\33[%uD", fc - tc);
    }
}

// Move the visible cursor so that it sits at the screen position
// corresponding to buffer index `target`. While the line is not being
// edited, it only records target as the cursor's index.
static void rln_sync_cursor_to(uint8_t target)
{
    if (!rln_editing)
    {
        rln_cur_idx = target;
        return;
    }
    // No prompt column (the terminal did not answer) → can't do row math.
    // Fall back to plain horizontal cursor moves and let the terminal wrap.
    if (rln_prompt_col == 0)
    {
        int delta = (int)target - (int)rln_cur_idx;
        if (delta > 0)
            com_printf("\33[%dC", delta);
        else if (delta < 0)
            com_printf("\33[%dD", -delta);
    }
    else if (rln_input_no_wrap())
    {
        // Single row: position by absolute column (CHA). A write into the
        // last column parks the terminal in pending-wrap; a relative move
        // would mis-measure from there, but CHA resolves to the real cell.
        // cursor_max keeps `target` off the rolled-over next row, so the
        // column is exact.
        uint8_t r;
        uint16_t c;
        rln_buf_to_screen(target, &r, &c);
        com_printf("\33[%uG", c);
    }
    else
    {
        uint8_t fr, tr;
        uint16_t fc, tc;
        rln_buf_to_screen(rln_cur_idx, &fr, &fc);
        rln_buf_to_screen(target, &tr, &tc);
        rln_emit_move(fr, fc, tr, tc);
        // A zero-length relative move emits nothing, so a prior margin write
        // left in pending-wrap (xenl) would survive; CHA to the same column
        // clears it, matching the no-wrap branch's unconditional CHA.
        if (fr == tr && fc == tc && tc == rln_width)
            com_printf("\33[%uG", tc);
    }
    rln_cur_idx = target;
}

// Effective max chars we can hold, given current screen geometry. In
// multi-line mode we cap at RLN_MAX_ROWS visible rows.
static uint8_t rln_effective_max(void)
{
    uint8_t cap = rln_max_length;
    if (rln_prompt_col)
    {
        uint16_t w = rln_width;
        // Compute the row*width product in 32 bits so a large pinned width
        // (tio_set_term_width accepts up to 65535) can't truncate; clamp
        // back into uint16_t before the row math.
        uint32_t cells = (uint32_t)RLN_MAX_ROWS * w;
        uint16_t avail = cells > UINT16_MAX ? UINT16_MAX : (uint16_t)cells;
        if (avail > (uint16_t)(rln_prompt_col - 1))
            avail -= (uint16_t)(rln_prompt_col - 1);
        else
            avail = 0;
        if (avail > 255)
            avail = 255;
        if (cap > avail)
            cap = (uint8_t)avail;
    }
    return cap;
}

// Highest reachable cursor index. Normally buflen; one less when the
// input is full and fills exactly to the last column, so the cursor
// rests on the pending-wrap cell instead of a phantom next row (which
// would drift relative moves up). The "full" guard keeps multi-line
// appending working — merely filling a row must still let you move on.
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

// Write `count` erase spaces starting at the current screen position (the
// cell just past rln_buflen), then back the cursor up to where it started.
// A trailing space written into the last column lands in pending-wrap and
// does not advance, so back up one fewer in that case.
static void rln_emit_trailing_spaces(uint8_t count)
{
    uint8_t r;
    uint16_t c;
    rln_buf_to_screen(rln_buflen, &r, &c);
    uint16_t back = count;
    if (c + count - 1 >= rln_width)
        back = (uint16_t)(count - 1);
    for (uint8_t i = 0; i < count; i++)
        com_putchar(' ');
    if (back)
        com_printf("\33[%uD", back);
}

// Walk down `rows` existing rows from the cursor, clearing each, then return up
// to the start row at column `col`. CUD (not LF) so rows above stay put.
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
//   - No-wrap: plain writes from `start` with space-fill erase. No EL, no
//     ICH, no DCH — these would clobber adjacent form data on the same row.
//   - Wrap: the input is one logical line — write it from the start and let
//     the terminal autowrap (the standard readline/linenoise model), then
//     clear the tail and any rows a previous longer render owned. Never
//     \33[J, which would erase rows below that rln does not own.
// Both erase an old tail only when the line got shorter, so typing at the end
// of the line echoes just the typed char.
static void rln_render_from(uint8_t start)
{
    if (!rln_editing)
        return;
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
        for (uint8_t i = start; i < rln_buflen; i++)
            com_putchar(rln_buf[i]);
        // The render leaves the cursor just past the last char. When that char
        // filled the final column the terminal is in pending-wrap. Two cases:
        //   - Full input pinned on the last cell (cursor_max < buflen): keep
        //     the cursor there. Do NOT commit the wrap with a SPACE — that
        //     autowraps to a phantom continuation row (a stray newline/scroll)
        //     with no next char coming. The closing sync's CHA clears the xenl;
        //     no EL, which at the margin would erase the char just written.
        //   - Otherwise: a margin-filling row is committed with SPACE+BS so it
        //     stays a soft-wrapped continuation a resize can reflow (a hard \n
        //     would split the logical line); EL then drops the space and any
        //     stale tail. A non-margin end needs EL only when the line got
        //     shorter, to drop the stale tail.
        uint8_t cmax = rln_cursor_max();
        uint8_t er;
        uint16_t ec;
        if (cmax < rln_buflen)
        {
            rln_buf_to_screen(cmax, &er, &ec);
            rln_cur_idx = cmax;
        }
        else
        {
            rln_buf_to_screen(rln_buflen, &er, &ec);
            bool filled_margin = rln_buflen > start && ec == 1;
            if (filled_margin)
                com_printf(" \b\33[K");
            else if (rln_last_render_buflen > rln_buflen)
                com_printf("\33[K");
            rln_cur_idx = rln_buflen;
        }
        // Clear rows a previous (longer) render owned below the new end,
        // then return to the new end so cur_idx tracks the screen.
        if (rln_rendered_max_row > er)
            rln_clear_rows_below((uint8_t)(rln_rendered_max_row - er), ec);
        rln_rendered_max_row = er;
        rln_last_render_buflen = rln_buflen;
    }
    rln_sync_cursor_to(rln_bufpos);
}

// Redraw the wrapping input after the terminal width changed under us
// (NAWS, or a CPR geometry refinement). The terminal reflowed our one
// logical line keeping the cursor on the same char, so home the cursor
// relative to the NEW width, clear the rows the old render owned, and
// full-redraw.
static void rln_resize_redraw(void)
{
    if (!rln_editing || rln_prompt_col == 0)
        return;
    rln_sync_cursor_to(0);
    uint8_t maxr = rln_rendered_max_row;
    com_printf("\33[K");
    rln_clear_rows_below(maxr, rln_prompt_col);
    rln_cur_idx = 0;
    rln_rendered_max_row = 0;
    rln_last_render_buflen = 0;
    rln_render_from(0);
}

/* ----- Tail-rewrite emit helper ----- */

// Emit the screen update for an in-place buffer change at position `at`.
// The buffer must already reflect the new state; leaves cursor at
// rln_bufpos. `pad` = trailing spaces to erase old positions past the new
// tail (0 for insert/in-place replace, N for delete-N). Wrap mode falls
// back to rln_render_from instead of per-row ICH/DCH.
static void rln_emit_tail_rewrite(uint8_t at, uint8_t pad)
{
    if (!rln_editing)
        return;
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
    // Keep recalled entries within the live cap so buflen <= effective_max
    // holds unconditionally (overwrite edits in place without a cap check).
    uint8_t cap = rln_effective_max();
    if (rln_buflen > cap)
    {
        rln_buflen = cap;
        rln_buf[rln_buflen] = 0;
    }
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

// Park the cursor past the input, nul-terminate the buffer, optionally
// push to history, and complete. The terminating newline is emitted in
// rln_complete (suppressed by the SUPPRESS_NL attribute).
static void rln_finish_line(bool add_to_history)
{
    rln_sync_cursor_to(rln_cursor_max());
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
        // moves. Write it and let the terminal autowrap at the margin; the
        // follow-up sync issues an explicit move that cancels pending-wrap.
        rln_buf[rln_bufpos] = ch;
        rln_action_taken = true;
        if (rln_editing)
        {
            rln_sync_cursor_to(rln_bufpos);
            uint8_t r;
            uint16_t c;
            rln_buf_to_screen(rln_bufpos, &r, &c);
            bool at_margin = (c == rln_width);
            com_putchar(ch);
            // The write advances the cursor past the cell, except at the
            // margin where pending-wrap leaves it parked on the cell.
            rln_cur_idx = at_margin ? rln_bufpos : (uint8_t)(rln_bufpos + 1);
            rln_bufpos++;
            rln_clamp_cursor();
            rln_sync_cursor_to(rln_bufpos);
        }
        else
        {
            rln_bufpos++;
            rln_clamp_cursor();
        }
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
// tio sends it only to the terminals taken to support shapes, so on the
// others the mode has no visual indicator.
static void rln_emit_mode_cursor(void)
{
    tio_cursor_shape(rln_overwrite ? 1 : 5);
}

static void rln_toggle_overwrite(void)
{
    rln_overwrite = !rln_overwrite;
    rln_action_taken = true;
    if (rln_editing)
        rln_emit_mode_cursor();
}

/* ----- Items -----
 *
 * tio hands over one item at a time: a character, or a whole escape
 * sequence, the terminals' answers already taken out. A typed item is
 * dispatched while the line is being edited, and held until then before. */

// Forward decls: ending the wait, or following tio once it is ready, starts
// the edit, which replays the held items through rln_item.
static void rln_end_wait(void);
static void rln_follow(void);
static void rln_item(com_source_t src, const char *item, size_t len);

static void rln_dispatch_C0(uint8_t *line_end, uint8_t ch)
{
    // Both CR and LF end a line, because a terminal sends CR and a file of
    // host text holds LF, and nothing rewrites either on the way in. A CR and
    // LF next to each other are one line ending, so the second is discarded.
    // The memory lasts exactly one dispatched byte, which is why "\r\r" is
    // two lines and why a blank line in a CRLF file survives.
    uint8_t was = *line_end;
    *line_end = 0;
    if ((ch == '\r' || ch == '\n') && was && ch != was)
        return;
    if (ch == '\r' || ch == '\n')
    {
        *line_end = ch;
        rln_action_taken = true;
        rln_finish_line(true);
    }
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
    // terminals); 0x02 / 0x06 are the ctrl-B / ctrl-F variants some
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

static void rln_dispatch_SS3(uint8_t ch)
{
    if (ch == 'F')
        rln_line_end();
    else if (ch == 'H')
        rln_line_home();
}

static void rln_dispatch_CSI(const tio_csi_t *csi)
{
    uint8_t ch = csi->final;
    if (ch == 'A')
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
    else if (ch == 'P')
        rln_line_delete_n(csi);
    else if (ch == '@')
        rln_line_insert_n(csi);
    else if (ch == 'X')
        rln_line_erase_n(csi);
    else if (ch == 'b' || ch == 2)
        rln_line_word(-1);
    else if (ch == 'f' || ch == 6)
        rln_line_word(1);
    else if (ch == '~')
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
        rln_dispatch_C0(&rln_line_ends[src], (uint8_t)item[0]);
    else if (item[1] == '[')
        rln_dispatch_CSI(csi);
    else if (item[1] == 'O')
        rln_dispatch_SS3((uint8_t)item[2]);
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

// Holds a typed item that arrives before the edit starts. A line end starts
// the edit as soon as tio_line_end lets geometry be ready, which is at once
// for a peer that feeds bytes without answering the CPRs; until then tio
// takes only answers, so nothing typed after the line end is held. An item
// that does not fit starts the edit at once with no prompt column, and is
// then taken after the held ones.
static void rln_hold(com_source_t src, const char *item, size_t len)
{
    if (rln_held_len + 2 + len > sizeof rln_held)
    {
        rln_end_wait();
        if (rln_callback)
            rln_item(src, item, len);
        return;
    }
    rln_held[rln_held_len++] = (uint8_t)src;
    rln_held[rln_held_len++] = (uint8_t)len;
    memcpy(&rln_held[rln_held_len], item, len);
    rln_held_len += (uint16_t)len;
    if (len == 1 && (item[0] == '\r' || item[0] == '\n'))
    {
        tio_line_end();
        rln_follow();
    }
}

// Takes one item from a source, or from the poke stream. Private CSIs and SS2
// are dropped, and the rest are typed. No dropped item becomes the last key.
static void rln_item(com_source_t src, const char *item, size_t len)
{
    tio_csi_t csi;
    bool is_csi = tio_csi_parse(item, len, &csi);
    if ((is_csi && csi.priv) || (len == 3 && item[1] == 'N'))
        return;
    if (rln_editing)
        rln_dispatch(src, item, len, &csi);
    else
        rln_hold(src, item, len);
}

/* ----- Starting and following the edit ----- */

// The line is still being edited: no held line end finished it.
static bool rln_still_reading(void)
{
    return rln_callback && rln_editing;
}

static void rln_enter_edit(void)
{
    rln_editing = true;
    rln_emit_mode_cursor();
    rln_cur_idx = 0;
    if (rln_buflen)
        rln_render_from(0);
    else
        rln_sync_cursor_to(rln_bufpos);
    // Replay the held items in the order they came. A line end among them
    // finishes the line, and the replay stops there.
    uint16_t held = rln_held_len;
    rln_held_len = 0;
    for (uint16_t at = 0;
         at < held && rln_still_reading();
         at += 2 + rln_held[at + 1])
        rln_item((com_source_t)rln_held[at], (const char *)&rln_held[at + 2],
                 rln_held[at + 1]);
}

// Follows tio's geometry. The edit starts once it is ready, and the line is
// drawn again when the prompt column or the width changes under it. A line
// laid out without a prompt column is never drawn again.
static void rln_follow(void)
{
    if (!rln_editing)
    {
        if (!tio_ready())
            return;
        rln_prompt_col = tio_prompt_col();
        rln_width = tio_get_term_width();
        rln_enter_edit();
        return;
    }
    uint16_t pcol = tio_prompt_col();
    uint16_t width = tio_get_term_width();
    if (pcol == rln_prompt_col && width == rln_width)
        return;
    rln_width = width;
    if (!rln_prompt_col)
        return;
    rln_prompt_col = pcol;
    rln_resize_redraw();
}

// Geometry is ready now, with no prompt column, and the held items are
// replayed. A line end among them finishes the line.
static void rln_end_wait(void)
{
    tio_ready_now();
    rln_follow();
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

    rln_editing = false;
    rln_prompt_col = 0;
    rln_cur_idx = 0;
    rln_rendered_max_row = 0;
    rln_last_render_buflen = 0;
    rln_held_len = 0;
    rln_line_src = COM_SOURCE_ANY;
}

void rln_read_line(rln_read_callback_t callback)
{
    rln_start(callback);
    tio_read_start(rln_max_length);
}

void rln_read_line_script(rln_read_callback_t callback, uint32_t timeout_ms)
{
    assert(timeout_ms);
    rln_start(callback);
    tio_read_start_unasked();
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
    if (!rln_callback)
        return false;
    if (!rln_editing)
    {
        rln_end_wait();
        if (!rln_still_reading())
            return true;
    }
    if (!rln_buflen)
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
    rln_editing = false;
    rln_overwrite = false;
    rln_held_len = 0;
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
        rln_sync_cursor_to(rln_cursor_max());
        com_printf("\r\n");
    }
    rln_init();
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
// position `target`, then advance rln_cur_idx past it. Caller must have
// already called rln_sync_cursor_to(target). The two cells autowrap like
// any other write; track where the cursor parks (on the cell at the margin).
static void rln_poke_emit_caret(uint8_t target, char ch)
{
    com_putchar('^');
    com_putchar((char)('@' + ch));
    rln_cur_idx = (uint8_t)(target + 2);
    if (rln_prompt_col)
    {
        uint16_t w = rln_width;
        uint16_t last_col = (uint16_t)((rln_prompt_col - 1 + target + 1) % w) + 1;
        if (last_col == w)
            rln_cur_idx = (uint8_t)(target + 1); // margin: caret parks on the cell
    }
}

// The string is framed into items as a source is. A sequence still open at the
// end of the string is dropped, and so is an overlong one.
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
        if (ch < 32 && ch != '\33' && ch != '\30')
        {
            if (!rln_editing)
            {
                rln_end_wait();
                if (!rln_still_reading())
                    break;
            }
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
