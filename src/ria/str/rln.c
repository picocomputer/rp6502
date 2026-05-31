/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "str/rln.h"
#include "str/str.h"
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
**   - Can't depend on auto-wrap; pending-wrap (xenl) is OK to rely on.
**   - rln owns only rln_max_length cells from the start of input, so
**     no ICH/DCH unless rln owns the entire visible input region.
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
    rln_phase_width_cpr,  // waiting for second CPR (height + width). DA2
                          // is sent between CPR1 and CPR2 in the burst, and
                          // is absorbed by the always-on CPR sniffer
                          // regardless of phase, so this phase just gates
                          // the CPR-driven transition to edit
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

// Per-input-source state. One instance per com source plus one for the
// 6502 poke stream. Owns the ANSI parser (state, csi params, buf[]) and
// per-source bookkeeping for CPR/DA2 tracking and deferred completion.
// buf[] doubles as in-flight cache (tail = currently in-progress
// sequence) and deferred-typed buffer (head = completed typed sequences
// whose dispatch is waiting for edit phase). Per-source bookkeeping
// fields are unused on rln_poke_source — poke is excluded from CPR/DA2
// tracking and from defer iteration.
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
static uint8_t rln_max_length;
static uint32_t rln_idle_timeout_ms;
static uint8_t rln_caps;
// 6502 attribute: when set, the line-terminating newline is suppressed
// so field input on the last line keeps the cursor on its row. Persists
// across reads; reset on stop() (via rln_init).
static bool rln_suppress_newline;

// Deferred completion. Set when rln_complete fires while at least one
// source is still busy (parser mid-ESC or owes proven CPR replies); the
// callback invocation is held off until every busy source resolves its
// arm-time criteria (in-flight ESC sequence back to C0 AND all owed
// CPRs landed) or RLN_COMPLETE_DEFER_MS elapses from defer arm. Sources
// not busy at arm are not read during the defer window — their bytes
// stay in com's per-source FIFO and arrive at the next rln_read_line
// as a fresh stream. Per-source defer fields live in rln_source_t and
// are snapshot at arm so each source's criteria depends only on what
// *that source* owed at arm.
static bool rln_complete_deferred;
static bool rln_complete_deferred_timed_out;
static absolute_time_t rln_complete_deferred_deadline;

// Cross-terminal display state
static rln_phase_t rln_phase;
static absolute_time_t rln_handshake_deadline;
static uint16_t rln_prompt_col;      // 1-based
static uint16_t rln_term_width;      // 0 if no CPR
static uint16_t rln_term_height;     // 0 if no CPR
static uint16_t rln_width_override;  // 0 = auto-detect
static uint16_t rln_height_override; // 0 = auto-detect
static uint8_t rln_cur_idx;          // buffer index whose screen position the cursor is at
static bool rln_overwrite;           // preserved across rln_read_line calls; cleared by rln_init (rln_stop/rln_break)
static bool rln_decscusr_ok;         // peer claimed VT220+ via Primary DA; safe to emit DECSCUSR
static uint8_t rln_rendered_max_row; // highest row index rln has written to in the current line
static uint8_t rln_rendered_end;     // buflen as of last render in no-wrap mode

// Per-source state. rln_sources[s] holds the parser + CPR/DA2/defer
// bookkeeping for each com input source (kbd, uart, tel). rln_poke_source
// is a separate instance because it is fed synchronously from rln_poke
// (the 6502 API) and has its own drop-cpr rule and overwrite handshake.
static rln_source_t rln_sources[COM_SOURCE_COUNT];
static rln_source_t rln_poke_source;

// CPR-pending count seeded into rln_sources[s].cpr_expecting at read
// start (1 or 2 depending on geometry overrides). cpr_seen is sticky
// across reads — once a source has dispatched a valid CPR it stays
// flagged so the defer gate keeps distinguishing real terminals from
// script peers that never reply. cpr_seen is cleared when the prior
// verdict is confirmed stale (rln_cpr_forget_stale call sites).
static uint8_t rln_cpr_initial;

// Sticky-off latch: when any source proves DA2-deaf this read (completed
// its CPR quota or hit the 1-CPR timeout without delivering DA2), forces
// rln_decscusr_ok back to false and blocks rln_da2_dispatch from
// re-enabling. Stdout writes go to every attached terminal, so a single
// VT102 peer would leak the `q` of DECSCUSR as literal text; one bad peer
// must poison cursor-shape support for everyone. Cleared per read.
static bool rln_decscusr_locked_off;

// Lastkey capture for the 6502 API. rln_action_taken is the live
// "this dispatch mutated state" flag, sampled at lastkey publish time.
// Typed and poked dispatches both populate these; CPR/DA/DA2 protocol
// replies don't touch lastkey.
static uint8_t rln_lastkey_buf[RLN_LASTKEY_MAX];
static uint8_t rln_lastkey_len;
static bool rln_action_taken;
static bool rln_lastkey_action;

// True if source s still owes rln in-band protocol work: mid a
// multi-byte ANSI sequence, or owes a CPR reply we've proven it can
// send. Classify by parser state rather than inflight_len: the CR or
// poked control byte that fires rln_complete is itself in the parser's
// buf with inflight_len == 1, but state is already back to C0 — a
// single-byte C0 dispatch must not force a defer. The cpr_seen gate
// keeps script peers (never reply) from pinning completion open on a
// speculative request. Poke is excluded by iteration bounds: rln_poke
// is gated off during defer, so a stuck poke partial would only pin
// the deadline for nothing.
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

// Called after every byte we feed from source s during defer. Clears
// defer_pending once *both* arm-time criteria are satisfied: the
// in-flight ESC sequence at arm has returned to C0, AND all outstanding
// CPRs have landed. ANDing is essential — typed sequences during defer
// (arrow keys etc.) cycle the parser through CSI back to C0 and would
// otherwise satisfy an "or" gate while CPRs are still in flight,
// leaking partial CPR fragments ("33R", "[11;33R") into the next read.
// defer_esc_pending is set only at arm; we don't re-arm it mid-defer.
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

// Immediate completion: invoke the user callback and clear all
// in-band completion state. Callers must have already verified that
// it's safe to drop in-flight parser tails.
static void rln_complete_now(bool timed_out)
{
    rln_read_callback_t cc = rln_callback;
    rln_idle_timeout_ms = 0;
    rln_callback = NULL;
    rln_complete_deferred = false;
    // End of input: forget the sticky cpr_seen verdict on any source
    // that didn't engage this round. Covers the fast-CR case (completion
    // before the handshake_deadline has elapsed) and idle-timeout
    // completion, both of which the deadline-driven handler in rln_task
    // misses. Run before the callback fires so a synchronous
    // rln_read_line from cc() sees this round's final cpr_expecting
    // state — not the next round's freshly-reset values.
    for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
        rln_cpr_forget_stale(s);
    // Emit the line-terminating newline here rather than at
    // rln_finish_line. Uploaders that key their next chunk off
    // seeing '\n' echo back use the newline as a flow-control
    // signal — emitting it before defer ends would tell a UART
    // script "ready for next chunk" while we're still pinned in
    // defer absorbing a sibling source's protocol tail, and the
    // next chunk would land in a com FIFO that nothing is draining.
    // The SUPPRESS_NL attribute drops it so field input on the last
    // line keeps the cursor on its row.
    if (!rln_suppress_newline)
        putchar('\n');
    cc(timed_out, rln_buf);
}

// Public completion entry. Fires inline when no source still owes
// in-band protocol work; defers otherwise so mid-ESC sequences and
// outstanding CPR replies can be absorbed by the gate in
// rln_ansi_dispatch_or_defer before the next rln_read_line memsets
// the parsers.
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

// Emit relative cursor moves from (fr,fc) to (tr,tc). For downward
// moves we use LF (\n): CUD doesn't scroll, so if the prompt has been
// pushed to the bottom of the screen, "down one row" via CUD silently
// no-ops and renders end up clobbering the same row. LF scrolls as
// needed. Forced wrap in rln_render_from leaves the cursor at
// deterministic positions — there is no xenl pending-wrap state to
// consume — so \r and \n are unambiguous.
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
        // Single row: position by absolute column from a CR anchor. A
        // write to the last column leaves the cursor in pending-wrap;
        // anchoring at column 1 and stepping forward places it exactly
        // regardless of that state, so left/backspace don't drift. The
        // end index lands at the margin; clamp there rather than letting
        // rln_buf_to_screen roll it over to a phantom next row.
        uint16_t w = rln_get_term_width();
        uint32_t logical = (uint32_t)(rln_prompt_col - 1) + target;
        uint16_t c = (logical >= w) ? w : (uint16_t)(logical + 1);
        putchar('\r');
        if (c > 1)
            printf("\33[%uC", c - 1);
    }
    else
    {
        uint8_t fr, tr;
        uint16_t fc, tc;
        rln_buf_to_screen(rln_cur_idx, &fr, &fc);
        rln_buf_to_screen(target, &tr, &tc);
        rln_emit_move(fr, fc, tr, tc);
    }
    rln_cur_idx = target;
}

// Redraw the buffer from `start` onward, then place the cursor at
// rln_bufpos. Two paths:
//   - No-wrap: plain writes with space-fill erase. No EL, no ICH, no DCH —
//     these would clobber adjacent form data on the same row.
//   - Wrap: per-row \33[K and forced wraps. Never \33[J, which would
//     erase rows below that rln does not own.
static void rln_render_from(uint8_t start)
{
    if (rln_phase != rln_phase_edit)
        return;
    rln_sync_cursor_to(start);
    if (rln_input_no_wrap())
    {
        for (uint8_t i = start; i < rln_buflen; i++)
            putchar(rln_buf[i]);
        rln_cur_idx = rln_buflen;
        if (rln_rendered_end > rln_buflen)
        {
            uint8_t spaces = (uint8_t)(rln_rendered_end - rln_buflen);
            for (uint8_t i = 0; i < spaces; i++)
                putchar(' ');
            printf("\33[%uD", spaces);
        }
        rln_rendered_end = rln_buflen;
    }
    else
    {
        // Clear the start row from the cursor onward, then write
        // chars. Each forced wrap clears the new row before writing
        // into it, so the only rows we ever touch are rln's own.
        uint16_t w = rln_get_term_width();
        uint8_t r;
        uint16_t c;
        rln_buf_to_screen(start, &r, &c);
        printf("\33[K");
        for (uint8_t i = start; i < rln_buflen; i++)
        {
            putchar(rln_buf[i]);
            if (c == w)
            {
                // Forced wrap so a shadow terminal of a different
                // width still breaks at the same logical column,
                // plus clear the new row before populating it.
                printf("\n\33[K");
                r++;
                c = 1;
            }
            else
                c++;
        }
        // Walk down through any rows a previous (longer) render
        // owned past the new end and clear each one. Then return
        // the cursor to the new end position so cur_idx stays in
        // sync with the screen.
        uint8_t r_end = r;
        while (r < rln_rendered_max_row)
        {
            printf("\n\r\33[K");
            r++;
        }
        if (r > r_end)
        {
            printf("\33[%uA\r", r - r_end);
            if (c > 1)
                printf("\33[%uC", c - 1);
        }
        rln_rendered_max_row = r_end;
        rln_cur_idx = rln_buflen;
        rln_rendered_end = rln_buflen;
    }
    rln_sync_cursor_to(rln_bufpos);
}

/* ----- Tail-rewrite emit helper ----- */

// Emit the screen update for an in-place buffer change at position `at`.
// The buffer must already reflect the new state. Leaves cursor at
// rln_bufpos. `pad` is the number of trailing spaces needed to erase
// old display positions past the new tail (0 for insert / in-place
// replace, N for delete-N). Wrap mode falls back to rln_render_from
// rather than per-row ICH/DCH — simpler, and bounded by RLN_MAX_ROWS.
static void rln_emit_tail_rewrite(uint8_t at, uint8_t pad)
{
    if (rln_phase != rln_phase_edit)
        return;
    if (!rln_input_no_wrap())
    {
        rln_render_from(at);
        return;
    }
    rln_sync_cursor_to(at);
    for (uint8_t i = at; i < rln_buflen; i++)
        putchar(rln_buf[i]);
    if (pad)
    {
        for (uint8_t i = 0; i < pad; i++)
            putchar(' ');
        printf("\33[%uD", pad);
    }
    rln_cur_idx = rln_buflen;
    rln_rendered_end = rln_buflen;
    rln_sync_cursor_to(rln_bufpos);
}

/* ----- History ----- */

static void rln_replace_buf_from_history(void)
{
    // Buffer just got swapped from history; cursor goes to end.
    rln_buflen = (uint8_t)strnlen(rln_buf, RLN_BUF_SIZE - 1);
    rln_bufpos = rln_buflen;
    rln_render_from(0);
}

// dir > 0 walks toward older entries (up arrow), dir < 0 toward newer.
static void rln_history_step(int dir)
{
    if (!rln_enable_history || rln_idle_timeout_ms)
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
    if (!rln_enable_history || rln_idle_timeout_ms)
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
// rln_complete_now (suppressed by the SUPPRESS_NL attribute). Used by
// both the typed CR dispatch and rln_poke's control-byte path.
static void rln_finish_line(bool add_to_history)
{
    rln_sync_cursor_to(rln_buflen);
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
    if (rln_bufpos != rln_buflen)
        rln_action_taken = true;
    rln_bufpos = rln_buflen;
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
    if (rln_bufpos != prev)
    {
        rln_action_taken = true;
        rln_sync_cursor_to(rln_bufpos);
    }
}

static void rln_line_forward(rln_source_t *a)
{
    if (a->csi_param_count > 1 && a->csi_param[1] != 1)
        return rln_line_word(1);
    int count = a->csi_param[0];
    if (count < 1)
        count = 1;
    rln_step(count);
}

static void rln_line_backward(rln_source_t *a)
{
    if (a->csi_param_count > 1 && a->csi_param[1] != 1)
        return rln_line_word(-1);
    int count = a->csi_param[0];
    if (count < 1)
        count = 1;
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
    uint16_t count = a->csi_param[0];
    if (count < 1)
        count = 1;
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

// Effective max chars we can hold, given current screen geometry. In
// multi-line mode we cap at RLN_MAX_ROWS visible rows.
static uint8_t rln_effective_max(void)
{
    uint8_t cap = rln_max_length;
    if (rln_prompt_col)
    {
        uint16_t w = rln_get_term_width();
        uint16_t avail = (uint16_t)RLN_MAX_ROWS * w;
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

// ICH (CSI Ps @): insert Ps blank chars at the cursor, shifting the tail
// right. Cursor stays at its current position. Symmetric with DCH.
static void rln_line_insert_n(rln_source_t *a)
{
    uint16_t count = a->csi_param[0];
    if (count < 1)
        count = 1;
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
    uint16_t count = a->csi_param[0];
    if (count < 1)
        count = 1;
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
        // Overwrite replaces one char in place; nothing past the
        // cursor moves, so we just sync the cursor, write the char,
        // and resolve xenl if we landed at the right margin.
        rln_buf[rln_bufpos] = ch;
        rln_action_taken = true;
        if (rln_phase == rln_phase_edit)
        {
            rln_sync_cursor_to(rln_bufpos);
            bool at_row_end = false;
            if (!rln_input_no_wrap())
            {
                uint8_t r;
                uint16_t c;
                rln_buf_to_screen(rln_bufpos, &r, &c);
                at_row_end = (c == rln_get_term_width());
            }
            putchar(ch);
            rln_bufpos++;
            rln_cur_idx = rln_bufpos;
            if (at_row_end)
                putchar('\n');
        }
        else
            rln_bufpos++;
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
        // terminator
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

// Trim the in-flight tail off buf[] (e.g. after dispatching a sequence
// or after CAN). Leaves deferred head bytes intact.
static void rln_ansi_trim_inflight(rln_source_t *a)
{
    a->buf_len -= a->inflight_len;
    a->inflight_len = 0;
}

// Snapshot the in-flight tail into rln_lastkey_buf. Called only from
// typed-action dispatch during edit phase.
static void rln_publish_lastkey(rln_source_t *a)
{
    uint8_t n = a->inflight_len > RLN_LASTKEY_MAX
                    ? RLN_LASTKEY_MAX
                    : (uint8_t)a->inflight_len;
    memcpy(rln_lastkey_buf, &a->buf[a->buf_len - a->inflight_len], n);
    rln_lastkey_len = n;
    rln_lastkey_action = rln_action_taken;
}

// Dispatch a parsed Secondary DA reply (\33[><...>c). The fact that
// a reply arrived at all is the signal — minicom and similarly broken
// peers ignore DA2 (and leak its `c` as literal text, scrubbed by the
// burst's DECRC + space + BS). Param values are discarded. A late DA2
// arriving in edit phase still flips the bit and emits the cursor
// shape so the user sees the correct insert/overwrite indicator going
// forward.
static void rln_da2_dispatch(com_source_t src)
{
    (void)src; // per-source da2_seen set in dispatch_or_defer above gate
    // Sticky-off latch from a peer that proved DA2-deaf this read:
    // ignore any DA2 (including a deferred one replayed via drain after
    // the latch fired) so cursor shapes can't be re-enabled.
    if (rln_decscusr_locked_off)
        return;
    // Late DA2 arriving after we've already entered edit phase with
    // geometry suppressed (instant relief or handshake fallback both
    // leave prompt_col at 0): ignore. Enabling DECSCUSR now would
    // emit a cursor-shape sequence into a stream whose peer never
    // negotiated VT220+ for this session.
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
    // Tracked terminal sources are the ones whose CPR/DA2 replies are
    // protocol responses we record. KBD sends typed user input that
    // could look like a CPR/DA2 but isn't a real reply; the poke
    // source is virtual (src=COM_SOURCE_ANY, an unindexable sentinel
    // into rln_sources[]). Everything CPR/DA2-related below gates on
    // this — protocol-state accounting, the lock-off latch, and the
    // rln_cpr_dispatch call.
    bool tracked = (src == COM_SOURCE_UART || src == COM_SOURCE_TEL);
    // Protocol-state updates that must run regardless of defer:
    //   - cpr_seen / cpr_expecting drive defer resolution and the
    //     lock-off detection below.
    //   - da2_seen feeds the lock-off check; every DA2 reply from a
    //     tracked source must be recorded, even those absorbed during
    //     defer.
    //   - rln_cpr_dispatch refines term_width / term_height from late
    //     or shadow-terminal CPRs (the manifold-min rule, documented
    //     in that function); it guards its own enter_edit/render
    //     calls against rln_complete_deferred so the completed line
    //     isn't re-emitted.
    //   - The 2-CPR lock-off latch likewise has to fire when a real
    //     CPR2 closes the quota, even if that CPR2 lands during defer.
    if (is_cpr && tracked)
    {
        rln_sources[src].cpr_seen = true;
        if (rln_sources[src].cpr_expecting > 0)
            rln_sources[src].cpr_expecting--;
    }
    if (is_da2 && tracked)
        rln_sources[src].da2_seen = true;
    // 2-CPR mode lock-off: if this CPR closed out the source's quota
    // (CPR1+CPR2 both delivered) and that same source never replied
    // DA2, the peer is DA2-deaf. The burst order is CPR1, DA2, CPR2,
    // so a DA2-aware peer would have replied DA2 before its CPR2.
    // Latch the global lock so any later DA2 (including one replayed
    // via drain) can't re-enable cursor shapes.
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
    // Completion is pending: the line is logically submitted and we're
    // only here to absorb in-band protocol bytes so they don't leak
    // into the next read. The pre-gate block above already ran every
    // effect we want during defer (CPR/DA2 accounting, lock-off,
    // geometry refinement); skip the DA2 enable + cursor emit and the
    // typed-action dispatch so the visual line stays stable.
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
        // CPR was dispatched above the gate; CSI_private (other
        // terminators) and SS2 are no-ops today. All three skip
        // publish_lastkey so protocol replies don't surface as
        // typed-key bytes via the 6502 lastkey API.
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

// Drain the deferred head region through the same parser. Called from
// rln_enter_edit. Re-feeds every byte currently in buf[]: protocol
// replies were trimmed at dispatch time during handshake, so what's
// left is typed bytes + any in-flight tail. Replaying preserves any
// in-flight state. Snapshots rln_callback at entry so that a CR in
// temp[] which completes the line (and may even synchronously start
// a new one) doesn't replay further stale bytes against the next
// line's freshly memset state.
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
        // Stop if the line finished (callback fired) or finished into a
        // defer. The defer path keeps rln_callback non-NULL but gates
        // dispatch — continuing the loop is wasted work and would defer
        // bytes into a line that's logically already submitted.
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
    // order: kbd (local human) → uart (local terminal) → tel (remote) →
    // poke (scripted input). dispatch_or_defer derives "is tracked"
    // from src so KBD and poke (src=COM_SOURCE_ANY) skip protocol
    // tracking automatically. Abort the cascade if a dispatched CR
    // mid-drain completes the line — any later drains would replay
    // stale bytes into the next line's parsers.
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
    // Handshake deadline expired. The init burst already RCP'd the
    // cursor back to the prompt and re-showed it, so we don't need
    // to fix cursor placement here. We never got both CPRs (CPR2
    // would have advanced us straight to edit), so drop into the
    // single-virtual-line model (row math bypassed by prompt_col==0)
    // and preserve whatever DA2 told us about DECSCUSR — except in
    // 1-CPR mode below, where the deadline is the only signal a peer
    // is DA2-deaf.
    //
    // 1-CPR mode lock-off: in 2-CPR mode the CPR2 sentinel triggers
    // the live latch in rln_ansi_dispatch_or_defer, but when both
    // geometry overrides pin the size we only send CPR1 — there's no
    // sentinel after DA2 in the burst, so we can't tell DA2-deaf from
    // DA2-in-flight until the deadline. Any source that delivered CPR1
    // (proving it's a real terminal that processes ANSI) without
    // having replied DA2 by now is DA2-deaf; latch off for everyone.
    if (rln_cpr_initial == 1)
        for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
            if (rln_sources[s].cpr_expecting == 0 && !rln_sources[s].da2_seen)
            {
                rln_decscusr_ok = false;
                rln_decscusr_locked_off = true;
                break;
            }
    for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
        rln_cpr_forget_stale(s);
    rln_prompt_col = 0;
    rln_term_height = 0;
    rln_term_width = 0;
    rln_enter_edit();
}

// Dispatch a parsed CPR (\33[<p1>;<p2>R). Row in p1, col in p2.
//
// Multi-source manifold rule: when several terminals are attached, the
// host's stdout fans out to all of them, and each replies with its own
// CPR1 (current = prompt position) then CPR2 (current = terminal size,
// after \33[999;999H). Replies arrive interleaved across com sources,
// but each source's pair stays in order. Classification:
//   1. First CPR ever (prompt_col == 0): pin prompt_col from p2 and
//      advance phase. prompt_col is the one axis where all terminals
//      must agree from the host's point of view — the host sent the
//      same prompt bytes to each — so the first reply wins.
//   2. Per-source COUNT picks out CPR2: the accounting block in
//      rln_ansi_dispatch_or_defer has already decremented this source's
//      cpr_expecting, so when it reads 0 here (and initial was 2),
//      this is the source's CPR2 (the geometry reply). Counting beats
//      the older "col > prompt_col" heuristic, which mis-discards a
//      legitimate CPR2 when the prompt sits at the terminal's right
//      edge (CPR2's col equals prompt_col there).
//   3. First-arriving CPR2 sets term_height / term_width and
//      transitions to edit phase; subsequent CPR2s from other sources
//      refine the running MIN — render width must be safe for the
//      narrowest connected terminal so the same byte stream wraps
//      identically on every attached terminal.
//   4. Per-source CPR1s after the first global reply are discarded —
//      their useful info (prompt position) was already captured in
//      step 1.
//   5. CPR after late-DA2 fallback (prompt_col deliberately 0 in
//      edit phase) is discarded entirely.
//
// Called from rln_ansi_dispatch_or_defer above the defer-absorb gate
// so late CPRs refine geometry even during defer. enter_edit and
// render_from are guarded against rln_complete_deferred — once the
// line is submitted we update internal geometry but never re-emit.
static void rln_cpr_dispatch(com_source_t src, uint16_t p1, uint16_t p2)
{
    // Step 5: discard CPRs after fallback left prompt_col at 0.
    if (rln_phase == rln_phase_edit && rln_prompt_col == 0)
        return;
    bool both_overrides = rln_width_override && rln_height_override;
    // Caller (dispatch_or_defer) only invokes this when src is a
    // tracked source (UART/TEL), so rln_sources[src] is a valid lookup.
    bool is_cpr2 = (rln_cpr_initial == 2 &&
                    rln_sources[src].cpr_expecting == 0);
    // Step 1: first CPR globally pins prompt_col.
    if (rln_prompt_col == 0)
    {
        rln_prompt_col = p2 ? p2 : 1;
        if (!rln_complete_deferred)
        {
            if (both_overrides)
                rln_enter_edit();
            else
                rln_phase = rln_phase_width_cpr;
        }
        return;
    }
    // Geometry fully pinned by overrides: ignore everything past CPR1.
    if (both_overrides)
        return;
    // Step 4: per-source CPR1 after the global first reply — discard.
    if (!is_cpr2)
        return;
    // Step 3: first CPR2 sets geometry and transitions to edit.
    if (rln_term_height == 0)
    {
        rln_term_height = p1;
        rln_term_width = p2;
        if (!rln_complete_deferred)
            rln_enter_edit();
        return;
    }
    // Step 3 (refinement): later CPR2s from other sources clamp the
    // running min across the manifold.
    bool width_changed = false;
    if (p2 < rln_term_width)
    {
        rln_term_width = p2;
        width_changed = true;
    }
    if (p1 < rln_term_height)
        rln_term_height = p1;
    if (width_changed &&
        rln_phase == rln_phase_edit &&
        !rln_complete_deferred)
        rln_render_from(0);
}

/* ----- Top-level task / lifecycle ----- */

void rln_read_line(rln_read_callback_t callback)
{
    rln_idle_timeout_ms = 0;
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
    // Sentinels for the content-based CPR heuristic: 0 means
    // "not pinned yet". Must be cleared every call or replies
    // from the next handshake get misclassified.
    rln_prompt_col = 0;
    rln_term_height = 0;
    rln_term_width = 0;
    rln_cur_idx = 0;
    rln_rendered_max_row = 0;
    rln_rendered_end = 0;
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
    //   s       DECSC saves prompt position
    //   6n      CPR1 (prompt column) — always sent
    // Optional DA2 probe (skipped when max_length == 0 so we don't
    // touch a column rln doesn't own):
    //   >c      DA2 — DECSCUSR-support probe; minicom mis-parses
    //           this and leaks the `c` as literal text at the
    //           prompt column
    //   u       DECRC snaps cursor back to prompt column
    //   ' \b'   write a space at the prompt column to overwrite any
    //           leaked `c`, then BS back. EL would be shorter but would
    //           clobber form data on the same row past the prompt.
    // Optional geometry probe (skipped only when BOTH overrides are
    // set — partial overrides still take CPR2 for the other axis):
    //   999;999H go to bottom-right
    //   6n       CPR2 (terminal height + width)
    //   u        DECRC back to prompt
    // Always tail:
    //   ?25h     show cursor
    // RCP runs from the burst rather than the CPR-reply path so the
    // cursor returns to the prompt even when the terminal doesn't
    // respond to CPR (pipes, dumb relays); the deadline fallback
    // inherits the right cursor placement for free. We do not touch
    // DECAWM; the user's terminal stays in its native wrap mode and
    // we trust the pending-wrap (xenl) model on margin writes.
    printf("\33[?25l\33[s\33[6n");
    if (rln_max_length > 0)
        printf("\33[>c\33[u \b");
    if (!(rln_width_override && rln_height_override))
        printf("\33[999;999H\33[6n\33[u");
    printf("\33[?25h");
}

void rln_read_line_timeout(rln_read_callback_t callback, uint32_t timeout_ms)
{
    assert(timeout_ms);
    rln_read_line(callback);
    rln_idle_timeout_ms = timeout_ms;
    rln_idle_deadline = make_timeout_time_ms(rln_idle_timeout_ms);
}

// Read one byte from the appropriate source(s). In normal operation
// src=NONE lets com_getchar pick via the sticky RX picker. During
// deferred completion we walk only the still-busy sources, leaving
// bytes on clean sources queued in their FIFOs to arrive at the next
// rln_read_line as a fresh stream. Returns -1 if no byte was available;
// *out_src is set to the source that delivered (or NONE on -1).
static int rln_read_next(com_source_t *out_src)
{
    *out_src = COM_SOURCE_ANY;
    if (!rln_complete_deferred)
        return com_getchar(out_src);
    for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
    {
        if (!rln_sources[s].defer_pending)
            continue;
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
        // Provide instant relief during handshake for scripting tools
        // that do not respond to ANSI sequences. CR pre-edit, or any
        // parser hitting its buf cap, means the peer is feeding bytes
        // without ever completing the CPR exchange — abandon the rest
        // of the handshake and enter edit phase so the line can flow.
        // Only fires pre-edit; once we enter edit phase subsequent
        // iterations skip this check.
        bool any_overflow = false;
        for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
            if (rln_sources[s].buf_len >= RLN_BUF_SIZE)
                any_overflow = true;
        if (rln_phase != rln_phase_edit && (ch == '\r' || any_overflow))
        {
            // Skip the rest of the CPR handshake. We never confirmed
            // geometry for this session, so drop prompt_col to force
            // no-wrap rendering (no row math at unverified widths).
            // rln_term_width is left as-is — a value captured by a
            // previous successful handshake is still useful for callers
            // that query rln_get_term_width(); it's only zeroed on a
            // real handshake timeout in rln_handshake_fallback().
            // Don't forget_stale here: the CR replay below fires
            // rln_complete, which needs sticky cpr_seen to keep
            // defer engaged so in-transit CPRs get absorbed instead
            // of leaking. rln_complete_now's end-of-input forget_stale
            // (and the handshake_deadline path) cover stale verdicts.
            rln_prompt_col = 0;
            rln_enter_edit();
        }
    }
    if (rln_callback && time_reached(rln_handshake_deadline))
    {
        if (rln_phase != rln_phase_edit)
            rln_handshake_fallback();
        else
            // CPR-success transitions to edit before the deadline, so the
            // fallback never fires for the happy path. Run forget_stale
            // directly here so a once-real-now-gone source can't keep
            // pinning rln_complete defer with its stale cpr_seen verdict.
            // Idempotent — harmless to re-run every tick past the deadline.
            for (com_source_t s = COM_SOURCE_KBD; s < COM_SOURCE_COUNT; s++)
                rln_cpr_forget_stale(s);
    }
    // Fire deferred completion before the idle-timeout check so a
    // timeout-triggered defer still resolves through its own path
    // instead of re-arming a fresh rln_complete(true). Fire early
    // when every source has met its arm-time criteria — no more
    // per-source resolution can land — so we don't burn the full
    // defer window waiting on a quiet that already happened. The
    // deadline remains the fallback for sequences that never land.
    if (rln_complete_deferred)
    {
        if (!rln_any_defer_pending() ||
            time_reached(rln_complete_deferred_deadline))
            rln_complete_now(rln_complete_deferred_timed_out);
    }
    if (rln_idle_timeout_ms && time_reached(rln_idle_deadline))
        rln_complete(true);
}

void rln_init(void)
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
        rln_sync_cursor_to(rln_buflen);
    rln_init();
    if (!ria_active())
        rln_emit_mode_cursor();
}

void rln_break(void)
{
    if (rln_callback)
        rln_sync_cursor_to(rln_buflen);
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
    if (rln_term_height > 0)
        return rln_term_height;
    if (vga_connected())
        return (vga_get_display_type() == 2) ? 32 : 30;
    return 24;
}

void rln_set_term_width(uint16_t v) { rln_width_override = v; }
void rln_set_term_height(uint16_t v) { rln_height_override = v; }

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
// already called rln_sync_cursor_to(target). In wrap mode we track the
// physical column and emit \n on margin overflow so the marker doesn't
// stretch outside the line's visible row.
static void rln_poke_emit_caret(uint8_t target, char ch)
{
    char marker[2] = {'^', (char)('@' + ch)};
    if (rln_input_no_wrap())
    {
        putchar(marker[0]);
        putchar(marker[1]);
    }
    else
    {
        uint16_t w = rln_get_term_width();
        uint16_t c = (uint16_t)((rln_prompt_col - 1 + target) % w) + 1;
        for (int i = 0; i < 2; i++)
        {
            putchar(marker[i]);
            if (c == w)
            {
                putchar('\n');
                c = 1;
            }
            else
                c++;
        }
    }
    rln_cur_idx = (uint8_t)(target + 2);
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
