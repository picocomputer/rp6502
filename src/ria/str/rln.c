/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "str/rln.h"
#include "sys/com.h"
#include "sys/ria.h"
#include "sys/vga.h"
#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_RLN)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

/* Console manifold compatibility rules.
**
** rln drives a "manifold" of terminals: primary UART + optional telnet
** shadow + optional VGA. Design goals: (1) secondary terminals wider
** than the primary still render correctly; (2) callers can place rln
** inside a wider form by setting rln_max_length to reserve only the
** cells rln is allowed to own.
**
** Runtime invariants (the handshake in rln_read_line is exempt — it
** uses \33[999;999H inside a DECSC/DECRC bracket to elicit CPR2):
**   - No absolute Y cursor movement; no writes past rln_max_length.
**   - The wrapping input is one logical line: rln writes it continuously
**     and lets the terminal autowrap (the standard readline/linenoise
**     model), so the terminal owns wrapping and resize reflow. rln keeps
**     its own row model and relies on pending-wrap (xenl) at the margin.
**   - rln owns only rln_max_length cells from the start of input, so
**     no ICH/DCH unless rln owns the entire visible input region.
**   - Dynamic resize (NAWS / CPR refinement) trusts the terminal to rewrap
**     on a width change; non-reflowing terminals aren't supported for it.
*/

typedef enum
{
    ansi_state_C0,
    ansi_state_Fe,
    ansi_state_SS2,
    ansi_state_SS3,
    ansi_state_CSI,
    ansi_state_CSI_private,
} rln_ansi_state_t;

typedef enum
{
    rln_phase_prompt_cpr, // waiting for first CPR (prompt position)
    rln_phase_width_cpr,  // waiting for second CPR (terminal height + width)
    rln_phase_edit,       // normal editing
} rln_phase_t;

#define RLN_BUF_SIZE 256
#define RLN_HISTORY_SIZE 8
#define RLN_CSI_PARAM_MAX_LEN 16
#define RLN_LASTKEY_MAX 32

// rln_caps modes — public ABI is uint8_t, so the values are wire values.
#define RLN_CAPS_OFF 0
#define RLN_CAPS_UPPER 1
#define RLN_CAPS_SWAP 2
// Terms < 29 cols may not reach 254/255 chars in 10 rows.
#define RLN_MAX_ROWS 10
// Longest wait before edit mode starts echoing characters. Long enough
// to absorb a telnet RTT for the CPR replies, short enough that a
// non-responsive peer doesn't feel like a hang.
#define RLN_HANDSHAKE_MS 250
// When line completion fires while a source still owes in-band protocol
// work (mid-ESC sequence or outstanding CPR replies), hold the callback
// off up to this many ms so those bytes can land and be absorbed instead
// of leaking into the next read. Hard deadline from the moment defer is
// armed; byte arrival does not extend it. Early-out via
// rln_any_defer_pending() is the happy path; this cap is the fallback
// for sequences that never finish landing.
#define RLN_COMPLETE_DEFER_MS 500

// Per-input-source state: one instance per com source plus one for the
// 6502 poke stream. Owns the ANSI parser and per-source CPR/DA2/defer
// bookkeeping. buf[] doubles as in-flight cache (tail = in-progress
// sequence) and deferred-typed buffer (head = completed sequences
// awaiting edit phase). Bookkeeping fields are unused on rln_poke_source.
typedef struct
{
    rln_ansi_state_t state;
    uint16_t csi_param[RLN_CSI_PARAM_MAX_LEN];
    uint8_t csi_param_count;
    uint8_t csi_private; // 0, or one of '<' '=' '>' '?'
    uint8_t buf[RLN_BUF_SIZE];
    uint16_t buf_len;
    uint16_t inflight_len;
    // Per-source bookkeeping (zero on rln_poke_source). cpr_seen is
    // sticky across reads — preserved by rln_read_line's per-read reset.
    uint8_t cpr_expecting;  // outstanding CPR count this read
    uint16_t cpr_w;         // max column this read = the screen right edge
    uint16_t cpr_h;         // max row this read = the screen bottom edge
    uint16_t cpr_pcol;      // min column this read = the prompt column
    bool cpr_seen;          // sticky: source has dispatched a valid CPR
    bool da2_seen;          // proven DA2-aware this read
    bool defer_pending;     // arm-time busy criteria still pending
    bool defer_esc_pending; // arm-time in-flight ESC sequence not yet done
} rln_source_t;

// History storage. The live edit buffer is rln_buf; history[0] is a
// scratch slot used to stash the user's unsubmitted line while they
// navigate older entries. history[1..RLN_HISTORY_SIZE-1] hold previous
// entries, newest at [1].
static char rln_history[RLN_HISTORY_SIZE][RLN_BUF_SIZE];
static uint8_t rln_history_count;
static uint8_t rln_history_pos;

// Input state
static char rln_buf[RLN_BUF_SIZE];
static rln_read_callback_t rln_callback;
static absolute_time_t rln_idle_deadline;
static uint8_t rln_buflen;
static uint8_t rln_bufpos;
static bool rln_enable_history;
static bool rln_skip_history;
static uint8_t rln_max_length;
static uint32_t rln_idle_timeout_ms;
static uint8_t rln_caps;
// 6502 attribute: when set, the line-terminating newline is suppressed
// so field input on the last line keeps the cursor on its row. Persists
// across reads; reset on stop() (via rln_init).
static bool rln_suppress_newline;

// Deferred completion. Set when rln_complete fires while a source is
// still busy (parser mid-ESC or owes proven CPR replies); the callback
// is held off until every busy source resolves its arm-time criteria
// (in-flight ESC back to C0 AND all owed CPRs landed) or
// RLN_COMPLETE_DEFER_MS elapses. Sources not busy at arm aren't read
// during the window — their bytes wait in com's FIFO for the next read.
// rln_poke_source is exempt (synchronous; never owes a handshake).
static bool rln_complete_deferred;
static bool rln_complete_deferred_timed_out;
static absolute_time_t rln_complete_deferred_deadline;

// Cross-terminal display state
static rln_phase_t rln_phase;
static absolute_time_t rln_handshake_deadline;
static uint16_t rln_prompt_col;        // 1-based
static uint16_t rln_term_width;        // 0 if no CPR
static uint16_t rln_term_height;       // 0 if no CPR
static uint16_t rln_width_override;    // 0 = auto-detect
static uint16_t rln_height_override;   // 0 = auto-detect
static uint16_t rln_naws_width;        // 0 = no telnet NAWS
static uint16_t rln_naws_height;       // 0 = no telnet NAWS
static uint8_t rln_cur_idx;            // buffer index whose screen position the cursor is at
static bool rln_overwrite;             // preserved across rln_read_line calls; cleared by rln_init (rln_stop/rln_break)
static bool rln_decscusr_ok;           // peer claimed VT220+ via Primary DA; safe to emit DECSCUSR
static uint8_t rln_rendered_max_row;   // highest row index rln has written to in the current line
static uint8_t rln_last_render_buflen; // buflen as of last render in no-wrap mode

// Per-source state: rln_sources[s] holds the parser + CPR/DA2/defer
// bookkeeping for each com input source (kbd, uart, tel). rln_poke_source
// is separate — fed synchronously from rln_poke (the 6502 API).
static rln_source_t rln_sources[COM_SOURCE_COUNT];
static rln_source_t rln_poke_source;

// CPR-pending count seeded into each source's cpr_expecting at read
// start (1 or 2, depending on geometry overrides).
static uint8_t rln_cpr_initial;

// Sticky-off latch: when any source proves DA2-deaf this read, forces
// rln_decscusr_ok false and blocks re-enabling. Stdout fans out to every
// terminal, so one VT102 peer would leak DECSCUSR's `q` as literal text —
// one bad peer must poison cursor shapes for all. Cleared per read.
static bool rln_decscusr_locked_off;

// Lastkey capture for the 6502 API. rln_action_taken is the live
// "this dispatch mutated state" flag, sampled at lastkey publish time.
// Typed and poked dispatches both populate these; CPR/DA/DA2 protocol
// replies don't touch lastkey.
static uint8_t rln_lastkey_buf[RLN_LASTKEY_MAX];
static uint8_t rln_lastkey_len;
static bool rln_action_taken;
static bool rln_lastkey_action;

// True if source s still owes in-band protocol work: mid a multi-byte
// ANSI sequence, or owes a proven CPR reply. cpr_seen keeps
// never-replying script peers from pinning completion open.
static bool rln_source_busy(com_source_t s)
{
    return rln_sources[s].state != ansi_state_C0 ||
           (rln_sources[s].cpr_seen && rln_sources[s].cpr_expecting > 0);
}

static bool rln_any_source_busy(void)
{
    for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
        if (rln_source_busy(s))
            return true;
    return false;
}

// Snapshot source s's busy reasons at defer-arm.
static void rln_defer_arm(com_source_t s)
{
    rln_source_t *a = &rln_sources[s];
    a->defer_pending = rln_source_busy(s);
    a->defer_esc_pending = (a->state != ansi_state_C0);
}

// Called after every byte fed from source s during defer. Clears
// defer_pending only once *both* arm-time criteria hold: the arm-time
// ESC sequence is back to C0 AND all owed CPRs have landed. AND (not
// OR): a typed sequence completing mid-defer must not release the gate
// while CPRs are still in flight. defer_esc_pending is set only at arm.
static void rln_defer_check_resolved(com_source_t s)
{
    rln_source_t *a = &rln_sources[s];
    if (!a->defer_pending)
        return;
    if (a->defer_esc_pending && a->state == ansi_state_C0)
        a->defer_esc_pending = false;
    if (!a->defer_esc_pending && a->cpr_expecting == 0)
        a->defer_pending = false;
}

static bool rln_any_defer_pending(void)
{
    for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
        if (rln_sources[s].defer_pending)
            return true;
    return false;
}

// Drop the sticky cpr_seen verdict for source s when it delivered no
// CPRs this round. A counted CPR (expecting decremented) means the
// source proved real this round and we keep its verdict.
static void rln_cpr_forget_stale(com_source_t s)
{
    if (rln_sources[s].cpr_expecting == rln_cpr_initial)
        rln_sources[s].cpr_seen = false;
}

static void rln_forget_all_stale(void)
{
    for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
        rln_cpr_forget_stale(s);
}

// Immediate completion: invoke the user callback and clear all
// in-band completion state. Callers must have already verified that
// it's safe to drop in-flight parser tails.
static void rln_complete_now(bool timed_out)
{
    rln_read_callback_t cc = rln_callback;
    rln_idle_timeout_ms = 0;
    rln_callback = NULL;
    rln_complete_deferred = false;
    // Forget stale cpr_seen verdicts before the callback fires, so a
    // synchronous rln_read_line() from cc() sees this round's final
    // cpr_expecting state. Covers fast-CR and idle-timeout completion,
    // which the deadline handler in rln_task misses.
    rln_forget_all_stale();
    // Emit the terminating newline here (not at rln_finish_line):
    // uploaders use the '\n' echo as a flow-control "ready for next
    // chunk" signal, so it must wait until defer ends. SUPPRESS_NL
    // drops it so last-line field input keeps the cursor on its row.
    if (!rln_suppress_newline)
        putchar('\n');
    cc(timed_out, rln_buf);
}

// Public completion entry. Fires inline when no source owes in-band
// work; defers otherwise so mid-ESC sequences and outstanding CPRs can
// be absorbed before the next rln_read_line resets the parsers.
static void rln_complete(bool timed_out)
{
    if (rln_complete_deferred)
        return;
    if (!rln_any_source_busy())
    {
        rln_complete_now(timed_out);
        return;
    }
    for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
        rln_defer_arm(s);
    rln_complete_deferred = true;
    rln_complete_deferred_timed_out = timed_out;
    rln_complete_deferred_deadline = make_timeout_time_ms(RLN_COMPLETE_DEFER_MS);
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
    uint16_t w = rln_get_term_width();
    return rln_prompt_col - 1 + rln_max_length <= w;
}

static void rln_buf_to_screen(uint8_t i, uint8_t *row, uint16_t *col)
{
    uint16_t w = rln_get_term_width();
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
            putchar('\n');
        printf("\r");
        if (tc > 1)
            printf("\33[%uC", tc - 1);
    }
    else if (tr < fr)
    {
        printf("\33[%uA\r", fr - tr);
        if (tc > 1)
            printf("\33[%uC", tc - 1);
    }
    else if (tc != fc)
    {
        if (tc > fc)
            printf("\33[%uC", tc - fc);
        else
            printf("\33[%uD", fc - tc);
    }
}

// Move the visible cursor so that it sits at the screen position
// corresponding to buffer index `target`. No-op outside edit phase.
static void rln_sync_cursor_to(uint8_t target)
{
    if (rln_phase != rln_phase_edit)
    {
        rln_cur_idx = target;
        return;
    }
    // No prompt column captured (CPR1 didn't reply) → can't do row math.
    // Fall back to plain horizontal cursor moves and let the terminal wrap.
    if (rln_prompt_col == 0)
    {
        int delta = (int)target - (int)rln_cur_idx;
        if (delta > 0)
            printf("\33[%dC", delta);
        else if (delta < 0)
            printf("\33[%dD", -delta);
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
        printf("\33[%uG", c);
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
        if (fr == tr && fc == tc && tc == rln_get_term_width())
            printf("\33[%uG", tc);
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
        uint16_t w = rln_get_term_width();
        // Compute the row*width product in 32 bits so a large width
        // override (rln_set_term_width accepts up to 65535) can't
        // truncate; clamp back into uint16_t before the row math.
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
        ((rln_prompt_col - 1u + rln_buflen) % rln_get_term_width()) == 0)
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
    if (c + count - 1 >= rln_get_term_width())
        back = (uint16_t)(count - 1);
    for (uint8_t i = 0; i < count; i++)
        putchar(' ');
    if (back)
        printf("\33[%uD", back);
}

// Walk down `rows` existing rows from the cursor, clearing each, then return up
// to the start row at column `col`. CUD (not LF) so rows above stay put.
static void rln_clear_rows_below(uint8_t rows, uint16_t col)
{
    for (uint8_t k = 0; k < rows; k++)
        printf("\33[B\r\33[K");
    if (rows)
    {
        printf("\33[%uA", rows);
        if (col > 1)
            printf("\33[%uC", col - 1);
    }
}

// Redraw the buffer, then place the cursor at rln_bufpos. Two paths:
//   - No-wrap: plain writes from `start` with space-fill erase. No EL, no
//     ICH, no DCH — these would clobber adjacent form data on the same row.
//   - Wrap: the input is one logical line — write it from the start and let
//     the terminal autowrap (the standard readline/linenoise model), then
//     clear the tail and any rows a previous longer render owned. Never
//     \33[J, which would erase rows below that rln does not own.
static void rln_render_from(uint8_t start)
{
    if (rln_phase != rln_phase_edit)
        return;
    rln_clamp_cursor();
    rln_sync_cursor_to(start);
    if (rln_input_no_wrap())
    {
        for (uint8_t i = start; i < rln_buflen; i++)
            putchar(rln_buf[i]);
        rln_cur_idx = rln_cursor_max();
        if (rln_last_render_buflen > rln_buflen)
            rln_emit_trailing_spaces((uint8_t)(rln_last_render_buflen - rln_buflen));
        rln_last_render_buflen = rln_buflen;
    }
    else
    {
        for (uint8_t i = start; i < rln_buflen; i++)
            putchar(rln_buf[i]);
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
        //     stale tail. A non-margin end just needs EL.
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
                printf(" \b\33[K");
            else
                printf("\33[K");
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
    if (rln_phase != rln_phase_edit || rln_prompt_col == 0)
        return;
    rln_sync_cursor_to(0);
    uint8_t maxr = rln_rendered_max_row;
    printf("\33[K");
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
    if (rln_phase != rln_phase_edit)
        return;
    rln_clamp_cursor();
    if (!rln_input_no_wrap())
    {
        rln_render_from(at);
        return;
    }
    rln_sync_cursor_to(at);
    for (uint8_t i = at; i < rln_buflen; i++)
        putchar(rln_buf[i]);
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
    if (!rln_enable_history || rln_idle_timeout_ms || rln_skip_history)
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
    if (!rln_enable_history || rln_idle_timeout_ms || rln_skip_history)
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
// rln_complete_now (suppressed by the SUPPRESS_NL attribute).
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

static void rln_line_forward(rln_source_t *a)
{
    if (a->csi_param_count > 1 && a->csi_param[1] != 1)
        return rln_line_word(1);
    int count = rln_param_or_1(a->csi_param[0]);
    rln_step(count);
}

static void rln_line_backward(rln_source_t *a)
{
    if (a->csi_param_count > 1 && a->csi_param[1] != 1)
        return rln_line_word(-1);
    int count = rln_param_or_1(a->csi_param[0]);
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

static void rln_line_delete_n(rln_source_t *a)
{
    uint16_t count = rln_param_or_1(a->csi_param[0]);
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
static void rln_line_insert_n(rln_source_t *a)
{
    uint16_t count = rln_param_or_1(a->csi_param[0]);
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
static void rln_line_erase_n(rln_source_t *a)
{
    uint16_t count = rln_param_or_1(a->csi_param[0]);
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
        if (rln_phase == rln_phase_edit)
        {
            rln_sync_cursor_to(rln_bufpos);
            uint8_t r;
            uint16_t c;
            rln_buf_to_screen(rln_bufpos, &r, &c);
            bool at_margin = (c == rln_get_term_width());
            putchar(ch);
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

// DECSCUSR (CSI Ps SP q) is only emitted when the peer has identified
// itself as VT220+ via Secondary DA. VT102 emulators (minicom, Linux
// console) mis-parse the intermediate space and leak the tail as
// literal characters, so on those terminals we silently keep the
// internal mode without any visual indicator.
static void rln_emit_mode_cursor(void)
{
    if (!rln_decscusr_ok)
        return;
    if (rln_overwrite)
        printf("\33[1 q");
    else
        printf("\33[5 q");
}

static void rln_toggle_overwrite(void)
{
    rln_overwrite = !rln_overwrite;
    rln_action_taken = true;
    if (rln_phase == rln_phase_edit)
        rln_emit_mode_cursor();
}

/* ----- ANSI parser -----
 *
 * Two-step model: advance + dispatch.
 *
 *   rln_ansi_advance() updates parser state and accumulates CSI params
 *   without invoking any action. When state returns to C0, the
 *   sequence is complete and rln_ansi_dispatch_or_defer() classifies
 *   it as CPR / DA2 / typed and either dispatches immediately
 *   (CPR/DA2 always; typed during edit phase) or leaves the bytes in
 *   buf[] for later replay (typed during handshake phase).
 */

// Forward decl: rln_cpr_dispatch -> rln_enter_edit -> drain -> feed ->
// dispatch_or_defer -> rln_cpr_dispatch is a real cycle.
static void rln_cpr_dispatch(com_source_t src, uint16_t p1, uint16_t p2);

static void rln_ansi_advance(rln_source_t *a, uint8_t ch)
{
    switch (a->state)
    {
    case ansi_state_C0:
        if (ch == '\33')
            a->state = ansi_state_Fe;
        // else: stay in C0; dispatch decides what to do with it
        return;
    case ansi_state_Fe:
        if (ch == '[')
        {
            a->state = ansi_state_CSI;
            a->csi_param_count = 0;
            a->csi_param[0] = 0;
            a->csi_private = 0;
        }
        else if (ch == 'N')
            a->state = ansi_state_SS2;
        else if (ch == 'O')
            a->state = ansi_state_SS3;
        else
            a->state = ansi_state_C0;
        return;
    case ansi_state_SS2:
        a->state = ansi_state_C0;
        return;
    case ansi_state_SS3:
        a->state = ansi_state_C0;
        return;
    case ansi_state_CSI:
    case ansi_state_CSI_private:
        if (isdigit(ch))
        {
            if (a->csi_param_count < RLN_CSI_PARAM_MAX_LEN)
            {
                uint32_t v = (uint32_t)a->csi_param[a->csi_param_count] * 10 + (ch - '0');
                a->csi_param[a->csi_param_count] = v > UINT16_MAX ? UINT16_MAX : (uint16_t)v;
            }
            return;
        }
        if (ch == ';' || ch == ':')
        {
            if (++a->csi_param_count < RLN_CSI_PARAM_MAX_LEN)
                a->csi_param[a->csi_param_count] = 0;
            else
                a->csi_param_count = RLN_CSI_PARAM_MAX_LEN;
            return;
        }
        if (ch == '<' || ch == '=' || ch == '>' || ch == '?')
        {
            a->state = ansi_state_CSI_private;
            a->csi_private = ch;
            return;
        }
        a->state = ansi_state_C0;
        if (a->csi_param_count < RLN_CSI_PARAM_MAX_LEN)
            a->csi_param_count++;
        return;
    }
}

static void rln_dispatch_C0(uint8_t ch)
{
    if (ch == '\r')
    {
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

static void rln_dispatch_CSI(rln_source_t *a, uint8_t ch)
{
    if (ch == 'A')
        rln_history_step(1);
    else if (ch == 'B')
        rln_history_step(-1);
    else if (ch == 'C')
        rln_line_forward(a);
    else if (ch == 'D')
        rln_line_backward(a);
    else if (ch == 'F')
        rln_line_end();
    else if (ch == 'H')
        rln_line_home();
    else if (ch == 'P')
        rln_line_delete_n(a);
    else if (ch == '@')
        rln_line_insert_n(a);
    else if (ch == 'X')
        rln_line_erase_n(a);
    else if (ch == 'b' || ch == 2)
        rln_line_word(-1);
    else if (ch == 'f' || ch == 6)
        rln_line_word(1);
    else if (ch == '~')
        // Function-key codes split across terminfo lineages: 1/4 = VT220-era
        // Find/Select repurposed as Home/End; 7/8 = rxvt/Linux console; 2/3
        // = Insert/Delete (universal). Keep both Home/End forms.
        switch (a->csi_param[0])
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

// Trim the in-flight tail off buf[], leaving deferred head bytes intact.
static void rln_ansi_trim_inflight(rln_source_t *a)
{
    a->buf_len -= a->inflight_len;
    a->inflight_len = 0;
}

// Snapshot the in-flight tail into rln_lastkey_buf.
static void rln_publish_lastkey(rln_source_t *a)
{
    uint8_t n = a->inflight_len > RLN_LASTKEY_MAX
                    ? RLN_LASTKEY_MAX
                    : (uint8_t)a->inflight_len;
    memcpy(rln_lastkey_buf, &a->buf[a->buf_len - a->inflight_len], n);
    rln_lastkey_len = n;
    rln_lastkey_action = rln_action_taken;
}

// Dispatch a parsed Secondary DA reply (\33[><...>c). The reply's mere
// arrival is the signal; param values are discarded. A late DA2 in edit
// phase still flips the bit and emits the cursor shape.
static void rln_da2_dispatch(com_source_t src)
{
    (void)src; // da2_seen is recorded by the caller
    // DA2-deaf latch set this read: ignore all DA2 (incl. a deferred one
    // replayed via drain) so cursor shapes can't be re-enabled.
    if (rln_decscusr_locked_off)
        return;
    // Late DA2 in edit phase with geometry suppressed (prompt_col==0 from
    // instant relief or handshake fallback): ignore — enabling DECSCUSR
    // now would emit into a stream whose peer never negotiated VT220+.
    if (rln_phase == rln_phase_edit && rln_prompt_col == 0)
        return;
    if (rln_decscusr_ok)
        return;
    rln_decscusr_ok = true;
    if (rln_phase == rln_phase_edit)
        rln_emit_mode_cursor();
}

// Classify the just-completed sequence and dispatch or defer.
// entry_state is the parser's state BEFORE this byte was advanced;
// term is this byte (the terminator).
static void rln_ansi_dispatch_or_defer(rln_source_t *a,
                                       com_source_t src,
                                       uint8_t term,
                                       rln_ansi_state_t entry_state)
{
    bool is_cpr = (entry_state == ansi_state_CSI &&
                   term == 'R' &&
                   a->csi_private == 0 &&
                   a->csi_param_count == 2);
    bool is_da2 = (entry_state == ansi_state_CSI_private &&
                   a->csi_private == '>' &&
                   term == 'c');
    // Tracked sources (UART/TEL) are the ones whose CPR/DA2 replies are
    // real protocol responses. KBD typed input can look like a reply but
    // isn't; the poke source is virtual (src=COM_SOURCE_ANY). All
    // CPR/DA2 handling below gates on this.
    bool tracked = (src == COM_SOURCE_UART || src == COM_SOURCE_TEL);
    // Protocol-state accounting (cpr_seen/cpr_expecting, da2_seen),
    // geometry refinement (rln_cpr_dispatch), and the lock-off latch all
    // run before the defer gate below: they must record every CPR/DA2,
    // including replies absorbed during defer.
    if (is_cpr && tracked)
    {
        rln_sources[src].cpr_seen = true;
        if (rln_sources[src].cpr_expecting > 0)
            rln_sources[src].cpr_expecting--;
    }
    if (is_da2 && tracked)
        rln_sources[src].da2_seen = true;
    // 2-CPR lock-off: if this CPR closes the source's quota and that
    // source never replied DA2, it's DA2-deaf — the burst order is
    // CPR1, DA2, CPR2, so a DA2-aware peer would have replied before its
    // CPR2. Latch the lock so later DA2s can't re-enable cursor shapes.
    if (rln_cpr_initial == 2 &&
        is_cpr && tracked &&
        rln_sources[src].cpr_expecting == 0 &&
        !rln_sources[src].da2_seen)
    {
        rln_decscusr_ok = false;
        rln_decscusr_locked_off = true;
    }
    if (is_cpr && tracked)
        rln_cpr_dispatch(src, a->csi_param[0], a->csi_param[1]);
    // Defer pending: the line is logically submitted; we're only
    // absorbing in-band protocol bytes so they don't leak into the next
    // read. The pre-gate block already ran the effects we want during
    // defer, so skip the DA2 enable and typed dispatch.
    if (rln_complete_deferred)
    {
        rln_ansi_trim_inflight(a);
        return;
    }
    if (is_da2)
    {
        rln_da2_dispatch(src);
    }
    else if (is_cpr ||
             entry_state == ansi_state_CSI_private ||
             entry_state == ansi_state_SS2)
    {
        // CPR dispatched above; CSI_private (other terminators) and SS2
        // are no-ops. All three skip publish_lastkey so protocol replies
        // don't surface via the 6502 lastkey API.
    }
    else if (rln_phase != rln_phase_edit)
    {
        // Typed action during handshake: leave bytes in buf head for
        // replay; reset inflight so the next byte starts fresh.
        a->inflight_len = 0;
        return;
    }
    else
    {
        rln_action_taken = false;
        switch (entry_state)
        {
        case ansi_state_C0:
            rln_dispatch_C0(term);
            break;
        case ansi_state_Fe:
            rln_dispatch_Fe(term);
            break;
        case ansi_state_SS3:
            rln_dispatch_SS3(term);
            break;
        case ansi_state_CSI:
            rln_dispatch_CSI(a, term);
            break;
        default:
            break;
        }
        rln_publish_lastkey(a);
    }
    rln_ansi_trim_inflight(a);
}

// Feed one byte through a per-stream parser. src identifies the parser
// to dispatch_or_defer, which uses it to gate CPR/DA2 tracking on
// tracked terminal sources (UART/TEL); KBD and the poke source
// (src=COM_SOURCE_ANY) skip the tracking but still parse normally.
static void rln_ansi_feed(rln_source_t *a, com_source_t src, uint8_t b)
{
    // CAN aborts any in-progress sequence with no dispatch.
    if (b == '\30')
    {
        rln_ansi_trim_inflight(a);
        a->state = ansi_state_C0;
        return;
    }
    // Append to in-flight cache. On overflow drop the byte — instant
    // relief in rln_task will trigger from the buf_len == cap check.
    if (a->buf_len >= sizeof a->buf)
        return;
    a->buf[a->buf_len++] = b;
    a->inflight_len++;

    rln_ansi_state_t entry_state = a->state;
    rln_ansi_advance(a, b);
    if (a->state == ansi_state_C0)
        rln_ansi_dispatch_or_defer(a, src, b, entry_state);
}

// Drain the deferred head region through the same parser (from
// rln_enter_edit). Re-feeds every buffered byte: handshake-time protocol
// replies were already trimmed, so what's left is typed bytes + any
// in-flight tail. Snapshots rln_callback so a CR that completes the line
// mid-drain doesn't replay stale bytes into the next line's parser.
static void rln_ansi_drain_deferred(rln_source_t *a, com_source_t src)
{
    if (a->buf_len == 0)
        return;
    rln_read_callback_t cb_at_start = rln_callback;
    uint8_t temp[RLN_BUF_SIZE];
    uint16_t total = a->buf_len;
    memcpy(temp, a->buf, total);
    a->state = ansi_state_C0;
    a->csi_param_count = 0;
    a->csi_param[0] = 0;
    a->csi_private = 0;
    a->buf_len = 0;
    a->inflight_len = 0;
    for (uint16_t i = 0; i < total; i++)
    {
        rln_ansi_feed(a, src, temp[i]);
        // Stop if the line finished (callback fired) or entered defer —
        // continuing would replay bytes into an already-submitted line.
        if (rln_callback != cb_at_start || rln_complete_deferred)
            return;
    }
}

/* ----- Phase transitions ----- */

static void rln_enter_edit(void)
{
    rln_phase = rln_phase_edit;
    rln_emit_mode_cursor();
    rln_cur_idx = 0;
    if (rln_buflen)
        rln_render_from(0);
    else
        rln_sync_cursor_to(rln_bufpos);
    // Drain each parser's deferred bytes through itself, in com_source_t
    // order (KBD, UART, TEL), then the poke parser separately below.
    // dispatch_or_defer gates protocol tracking on src, so KBD and poke
    // (src=COM_SOURCE_ANY) skip it. Abort the cascade if a dispatched CR
    // mid-drain completes the line — any later drains would replay stale
    // bytes into the next line's parsers.
    rln_read_callback_t cb_at_start = rln_callback;
    for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
    {
        rln_ansi_drain_deferred(&rln_sources[s], s);
        if (rln_callback != cb_at_start || rln_complete_deferred)
            return;
    }
    bool prev_overwrite = rln_overwrite;
    rln_overwrite = true;
    rln_ansi_drain_deferred(&rln_poke_source, COM_SOURCE_ANY);
    bool need_reemit = (rln_overwrite != prev_overwrite);
    rln_overwrite = prev_overwrite;
    if (need_reemit)
        rln_emit_mode_cursor();
}

static void rln_handshake_fallback(void)
{
    // Handshake deadline expired without both CPRs. The init burst
    // already RCP'd the cursor back, so drop into the single-virtual-line
    // model (row math bypassed by prompt_col==0).
    //
    // 1-CPR lock-off: with both geometry axes overridden we send only
    // CPR1, so there's no CPR2 sentinel to trigger the live latch — the
    // deadline is the only DA2-deaf signal. Any source that delivered
    // CPR1 but never replied DA2 is DA2-deaf; latch off for everyone.
    if (rln_cpr_initial == 1)
        for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
            if (rln_sources[s].cpr_expecting == 0 && !rln_sources[s].da2_seen)
            {
                rln_decscusr_ok = false;
                rln_decscusr_locked_off = true;
                break;
            }
    rln_forget_all_stale();
    rln_prompt_col = 0;
    rln_term_height = 0;
    rln_term_width = 0;
    rln_enter_edit();
}

// Dispatch a parsed CPR (\33[<p1>;<p2>R). Row in p1, col in p2.
//
// Classified by content, not arrival order, so a reply that arrives first or
// leaks from a previous read (the fast-paste failure) is still sorted right.
// \33[999;999H clamps the geometry reply to the bottom-right, so for one
// source the geometry reply is the MAX row/col and the prompt reply sits
// inside it (the MIN col). A source's size is trusted only once it has
// answered its full quota (cpr_expecting == 0) AND its edge is a real width
// past the prompt (max col > min col) — never a stray prompt reply collapsing
// width to ~1. The highest-priority qualified terminal supplies the size —
// telnet CPR over uart CPR (NAWS, when set, outranks both in
// rln_get_term_width); prompt_col is pinned once from the first qualifier.
// Until one source qualifies the size stays unknown and the handshake
// deadline drops to no-wrap.
//
// Called above the defer gate so a leaked reply still updates the span;
// enter_edit/resize are guarded against rln_complete_deferred.
static void rln_cpr_dispatch(com_source_t src, uint16_t p1, uint16_t p2)
{
    if (rln_phase == rln_phase_edit && rln_prompt_col == 0)
        return;
    bool both_overrides = rln_width_override && rln_height_override;
    // Both axes overridden: only CPR1 is sent, so the lone reply is the
    // prompt and the size comes from the overrides.
    if (both_overrides)
    {
        if (rln_prompt_col == 0)
        {
            rln_prompt_col = p2 ? p2 : 1;
            if (!rln_complete_deferred)
                rln_enter_edit();
        }
        return;
    }
    // Widen this source's column span: edge (max col) and prompt (min col).
    rln_source_t *a = &rln_sources[src];
    if (p2 > a->cpr_w)
        a->cpr_w = p2;
    if (a->cpr_pcol == 0 || p2 < a->cpr_pcol)
        a->cpr_pcol = p2 ? p2 : 1;
    if (p1 > a->cpr_h)
        a->cpr_h = p1;
    if (rln_phase == rln_phase_prompt_cpr && !rln_complete_deferred)
        rln_phase = rln_phase_width_cpr;
    // Highest-priority qualified terminal wins: telnet CPR over uart CPR. A
    // source qualifies once it has answered its quota (cpr_expecting == 0) with
    // a real edge past the prompt (cpr_w > cpr_pcol). KBD sends no real CPR so
    // it never qualifies. No longer a min across terminals.
    rln_source_t *pick = NULL;
    if (rln_sources[COM_SOURCE_TEL].cpr_expecting == 0 &&
        rln_sources[COM_SOURCE_TEL].cpr_w > rln_sources[COM_SOURCE_TEL].cpr_pcol)
        pick = &rln_sources[COM_SOURCE_TEL];
    else if (rln_sources[COM_SOURCE_UART].cpr_expecting == 0 &&
             rln_sources[COM_SOURCE_UART].cpr_w > rln_sources[COM_SOURCE_UART].cpr_pcol)
        pick = &rln_sources[COM_SOURCE_UART];
    if (!pick)
        return;
    uint16_t w = pick->cpr_w, h = pick->cpr_h, pc = pick->cpr_pcol;
    uint16_t old_w = rln_get_term_width();
    if (rln_prompt_col == 0)
        rln_prompt_col = pc;
    rln_term_height = h;
    rln_term_width = w;
    if (rln_complete_deferred)
        return;
    if (rln_phase != rln_phase_edit)
        rln_enter_edit();
    else if (rln_get_term_width() != old_w)
        rln_resize_redraw();
}

/* ----- Top-level task / lifecycle ----- */

void rln_read_line(rln_read_callback_t callback)
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
    rln_complete_deferred = false;

    rln_phase = rln_phase_prompt_cpr;
    // prompt_col == 0 is the per-read sentinel: the first CPR pins it, and
    // it gates the no-wrap fallback. Geometry is re-derived from this read's
    // CPRs, so clear it too rather than carry a stale size.
    rln_prompt_col = 0;
    rln_term_height = 0;
    rln_term_width = 0;
    rln_cur_idx = 0;
    rln_rendered_max_row = 0;
    rln_last_render_buflen = 0;
    rln_decscusr_ok = false;
    rln_decscusr_locked_off = false;
    // Reset per-read source state, preserving cpr_seen (sticky across
    // reads — a source that proved itself a real terminal last round
    // still owes us CPRs this round, so defer must engage even before
    // the first CPR of this round arrives).
    bool sticky_cpr_seen[COM_SOURCE_COUNT];
    for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
        sticky_cpr_seen[s] = rln_sources[s].cpr_seen;
    memset(rln_sources, 0, sizeof rln_sources);
    memset(&rln_poke_source, 0, sizeof rln_poke_source);
    // CPR1 always sent; CPR2 sent only when at least one geometry axis
    // isn't overridden. Seed expecting for every source — bytes go to
    // all attached terminals and any of them may reply. Non-terminal
    // sources have cpr_seen=false so they never block defer regardless.
    rln_cpr_initial = (rln_width_override && rln_height_override) ? 1 : 2;
    for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
    {
        rln_sources[s].cpr_seen = sticky_cpr_seen[s];
        rln_sources[s].cpr_expecting = rln_cpr_initial;
    }
    rln_handshake_deadline = make_timeout_time_ms(RLN_HANDSHAKE_MS);

    // Build the handshake burst piecewise. Common framing:
    //   ?25l    hide cursor
    //   ?7h     enable autowrap (the wrap render relies on it)
    //   7       DECSC saves prompt position
    //   6n      CPR1 (prompt column) — always sent
    // Optional DA2 probe (skipped when max_length == 0 so we don't
    // touch a column rln doesn't own):
    //   >c      DA2 — DECSCUSR probe; minicom leaks the `c` as literal
    //           text at the prompt column
    //   8       DECRC snaps cursor back to prompt column
    //   ' \b'   space over any leaked `c`, then BS back (not EL — EL
    //           would clobber form data past the prompt)
    // Optional geometry probe (skipped only when BOTH overrides are set):
    //   999;999H go to bottom-right
    //   6n       CPR2 (terminal height + width)
    //   8        DECRC back to prompt
    // Always tail:
    //   ?25h     show cursor
    // DECRC runs from the burst (not the CPR-reply path) so the cursor
    // returns to the prompt even when the peer ignores CPR; the deadline
    // fallback then inherits the right placement. ?7h enables autowrap:
    // the one-logical-line render needs the terminal to wrap (a telnet
    // client never gets the boot soft-reset, so don't assume its default).
    // DECAWM is forced on every read and left on by contract — cooked-input
    // callers accept it; rln does not save or restore the prior state.
    printf("\33[?25l\33[?7h\0337\33[6n");
    if (rln_max_length > 0)
        printf("\33[>c\0338 \b");
    if (!(rln_width_override && rln_height_override))
        printf("\33[999;999H\33[6n\0338");
    printf("\33[?25h");
}

void rln_read_line_timeout(rln_read_callback_t callback, uint32_t timeout_ms)
{
    assert(timeout_ms);
    rln_read_line(callback);
    rln_idle_timeout_ms = timeout_ms;
    rln_idle_deadline = make_timeout_time_ms(rln_idle_timeout_ms);
}

void rln_read_line_no_history(rln_read_callback_t callback)
{
    rln_read_line(callback);
    rln_skip_history = true;
}

// Read one byte from the appropriate source(s). In normal operation
// src=COM_SOURCE_ANY lets com_getchar pick via the sticky RX picker.
// During deferred completion we walk only the still-busy sources, leaving
// bytes on clean sources queued in their FIFOs to arrive at the next
// rln_read_line as a fresh stream. Returns -1 if no byte was available;
// *out_src is set to the source that delivered (or COM_SOURCE_ANY on -1).
static int rln_read_next(com_source_t *out_src)
{
    *out_src = COM_SOURCE_ANY;
    if (!rln_complete_deferred)
        return com_getchar(out_src);
    for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
    {
        if (!rln_sources[s].defer_pending)
            continue;
        // At rest but still owing a CPR: the next byte is either the ESC that
        // begins the reply (absorb it) or the next pasted line's typed input.
        // Peek so typed bytes stay FIFO-queued for the next read; yield the
        // source so completion fires instead of waiting out the defer deadline.
        if (rln_sources[s].state == ansi_state_C0)
        {
            int p = com_peekchar(s);
            if (p < 0)
                continue; // reply not here yet — keep the gate open
            if (p != '\33')
            {
                rln_sources[s].defer_pending = false;
                continue;
            }
        }
        com_source_t arg = s;
        int c = com_getchar(&arg);
        if (c >= 0)
        {
            *out_src = s;
            return c;
        }
    }
    return -1;
}

void rln_task(void)
{
    if (!rln_callback)
        return;
    while (rln_callback)
    {
        com_source_t this_src;
        int c = rln_read_next(&this_src);
        if (c < 0)
            break;
        char ch = (char)c;
        rln_idle_deadline = make_timeout_time_ms(rln_idle_timeout_ms);
        if (this_src != COM_SOURCE_ANY)
            rln_ansi_feed(&rln_sources[this_src], this_src, (uint8_t)ch);
        if (rln_complete_deferred && this_src != COM_SOURCE_ANY)
            rln_defer_check_resolved(this_src);
        // Instant relief for scripting tools that don't answer ANSI: a
        // pre-edit CR, or any parser hitting its buf cap, means the peer
        // is feeding bytes without completing the CPR exchange — abandon
        // the handshake and enter edit phase so the line can flow.
        bool any_overflow = false;
        for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
            if (rln_sources[s].buf_len >= RLN_BUF_SIZE)
                any_overflow = true;
        if (rln_phase != rln_phase_edit && (ch == '\r' || any_overflow))
        {
            // Skip the rest of the handshake: geometry was never
            // confirmed, so drop prompt_col to force no-wrap rendering.
            // term_width is kept (still useful for rln_get_term_width;
            // only a real timeout zeroes it). Don't forget_stale here —
            // the CR replay fires rln_complete, which needs sticky
            // cpr_seen to keep defer engaged so in-transit CPRs are
            // absorbed; complete_now and the deadline path clear stale
            // verdicts.
            rln_prompt_col = 0;
            rln_enter_edit();
        }
    }
    if (rln_callback && time_reached(rln_handshake_deadline))
    {
        if (rln_phase != rln_phase_edit)
            rln_handshake_fallback();
        else
            // Happy path already in edit before the deadline. Run
            // forget_stale here so a once-real-now-gone source can't pin
            // defer with a stale cpr_seen verdict. Idempotent.
            rln_forget_all_stale();
    }
    // Fire deferred completion before the idle-timeout check so a
    // timeout-triggered defer resolves through its own path, not a fresh
    // rln_complete(true). Fire early once every source met its arm-time
    // criteria; the deadline is the fallback for bytes that never land.
    if (rln_complete_deferred)
    {
        if (!rln_any_defer_pending() ||
            time_reached(rln_complete_deferred_deadline))
            rln_complete_now(rln_complete_deferred_timed_out);
    }
    if (rln_idle_timeout_ms && time_reached(rln_idle_deadline))
        rln_complete(true);
}

void __in_flash("rln_init") rln_init(void)
{
    rln_callback = NULL;
    rln_enable_history = true;
    rln_max_length = 255; // fits in 2^8 with nul
    rln_caps = RLN_CAPS_OFF;
    rln_suppress_newline = false;
    rln_phase = rln_phase_edit;
    rln_overwrite = false;
    rln_width_override = 0;
    rln_height_override = 0;
    rln_complete_deferred = false;
    memset(rln_sources, 0, sizeof rln_sources);
    memset(&rln_poke_source, 0, sizeof rln_poke_source);
    rln_decscusr_locked_off = false;
}

void rln_run(void)
{
    rln_enable_history = false;
    rln_max_length = 254; // reserve 1 for the stdin newline
    if (rln_decscusr_ok && !ria_active())
        printf("\33[0 q");
}

void rln_stop(void)
{
    if (rln_callback)
        rln_sync_cursor_to(rln_cursor_max());
    rln_init();
    if (!ria_active())
        rln_emit_mode_cursor();
}

void rln_break(void)
{
    if (rln_callback)
        rln_sync_cursor_to(rln_cursor_max());
    rln_init();
}

/* 6502 applications may configure the max length */

void rln_set_max_length(uint8_t v) { rln_max_length = v; }
uint8_t rln_get_max_length(void) { return rln_max_length; }

void rln_set_caps(uint8_t v) { rln_caps = v; }
uint8_t rln_get_caps(void) { return rln_caps; }

void rln_set_suppress_nl(uint8_t v) { rln_suppress_newline = v; }
uint8_t rln_get_suppress_nl(void) { return rln_suppress_newline; }

uint16_t rln_get_term_width(void)
{
    if (rln_width_override)
        return rln_width_override;
    if (rln_naws_width)
        return rln_naws_width;
    if (rln_term_width > 0)
        return rln_term_width;
    if (vga_connected())
    {
        vga_canvas_t c = vga_get_canvas();
        return (c == vga_canvas_320_240 || c == vga_canvas_320_180) ? 40 : 80;
    }
    return 80;
}

uint16_t rln_get_term_height(void)
{
    if (rln_height_override)
        return rln_height_override;
    if (rln_naws_height)
        return rln_naws_height;
    if (rln_term_height > 0)
        return rln_term_height;
    if (vga_connected())
        return (vga_get_display_type() == 2) ? 32 : 30;
    return 24;
}

void rln_set_term_width(uint16_t v) { rln_width_override = v; }
void rln_set_term_height(uint16_t v) { rln_height_override = v; }

void rln_set_naws_size(uint16_t w, uint16_t h)
{
    if (w == rln_naws_width && h == rln_naws_height)
        return;
    uint16_t old_w = rln_get_term_width();
    rln_naws_width = w;
    rln_naws_height = h;
    if (rln_callback &&
        rln_phase == rln_phase_edit &&
        rln_prompt_col != 0 &&
        !rln_complete_deferred &&
        rln_buflen > 0 &&
        rln_get_term_width() != old_w)
        rln_resize_redraw();
}

/* Readline magic: lastkey + peekpoke
 */

// int ria_readline_lastkey(char *key, unsigned char *action);
// xstack reply shape (only when at least one key byte is buffered): key
// bytes pushed in reverse so they pop in receive order, then the action
// byte last so it pops first. When no key is buffered nothing is pushed
// and the AX return is 0; the 6502 stub keys off AX before popping.
bool rln_api_lastkey(void)
{
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
        if (!api_push_uint8(&rln_lastkey_buf[i]))
            return api_return_errno(API_EINVAL);
    if (len && !api_push_uint8(&action))
        return api_return_errno(API_EINVAL);
    return api_return_ax(len);
}

// int ria_readline_peek(char *peek, unsigned char *pos);
bool rln_api_peek(void)
{
    if (!rln_callback)
        return api_return_ax(0);
    rln_buf[rln_buflen] = 0;
    for (int i = rln_buflen - 1; i >= 0; i--)
        if (!api_push_char(&rln_buf[i]))
            return api_return_errno(API_EINVAL);
    if (!api_push_uint8(&rln_bufpos))
        return api_return_errno(API_EINVAL);
    return api_return_ax(rln_buflen);
}

// Emit a "^X" caret-notation marker for the C0 byte `ch` at buffer
// position `target`, then advance rln_cur_idx past it. Caller must have
// already called rln_sync_cursor_to(target). The two cells autowrap like
// any other write; track where the cursor parks (on the cell at the margin).
static void rln_poke_emit_caret(uint8_t target, char ch)
{
    putchar('^');
    putchar((char)('@' + ch));
    rln_cur_idx = (uint8_t)(target + 2);
    if (rln_prompt_col)
    {
        uint16_t w = rln_get_term_width();
        uint16_t last_col = (uint16_t)((rln_prompt_col - 1 + target + 1) % w) + 1;
        if (last_col == w)
            rln_cur_idx = (uint8_t)(target + 1); // margin: caret parks on the cell
    }
}

// Any C0 control byte (0x00-0x1F) in the poked stream finishes the
// input, except for ESC (\33) which starts a CSI sequence and CAN
// (\30) which the parser uses to abort an in-flight sequence and
// reset state. CR (\r) completes and is added to history. Every other
// control (including LF, which echoes as ^J) completes without history
// and echoes as caret notation (^@..^_). The terminating newline is
// suppressed by the SUPPRESS_NL attribute, not by any particular byte.
void rln_poke(const char *str)
{
    if (!rln_callback)
        return;
    // The line is already submitted from the user's perspective once
    // completion is deferred; a second poke must not mutate the buffer
    // or chain another rln_finish_line into the quiet window.
    if (rln_complete_deferred)
        return;
    bool sync = (rln_phase == rln_phase_edit);
    bool prev_overwrite = rln_overwrite;
    if (sync)
        rln_overwrite = true;
    for (const char *p = str; *p; p++)
    {
        uint8_t ch = (uint8_t)*p;
        if (ch < 32 && ch != '\33' && ch != '\30')
        {
            if (ch != '\r' && rln_max_length >= 2)
            {
                uint8_t target = rln_bufpos;
                if (target > (uint8_t)(rln_max_length - 2))
                    target = (uint8_t)(rln_max_length - 2);
                rln_sync_cursor_to(target);
                rln_poke_emit_caret(target, (char)ch);
            }
            rln_finish_line(ch == '\r');
            break;
        }
        rln_ansi_feed(&rln_poke_source, COM_SOURCE_ANY, (uint8_t)ch);
    }
    if (sync)
    {
        bool need_reemit = (rln_overwrite != prev_overwrite);
        rln_overwrite = prev_overwrite;
        if (need_reemit)
            rln_emit_mode_cursor();
    }
}

// int ria_readline_poke(const char *poke);
bool rln_api_poke(void)
{
    const char *poke = (char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    rln_poke(poke);
    return api_return_ax(0);
}
