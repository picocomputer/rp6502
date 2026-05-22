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
#define RLN_HANDSHAKE_MS 250
#define RLN_MAX_ROWS 10

// Unified CSI parser. One instance per input stream (local RX, telnet
// RX, 6502 poke) so a partial sequence on one stream can't be sliced
// by bytes from another. The buf[] field doubles as in-flight cache
// (tail = the bytes of the currently in-progress sequence) and
// deferred-typed buffer (head = completed typed sequences whose
// dispatch is waiting for edit phase).
typedef struct
{
    rln_ansi_state_t state;
    uint16_t csi_param[RLN_CSI_PARAM_MAX_LEN];
    uint8_t csi_param_count;
    uint8_t csi_private; // 0, or one of '<' '=' '>' '?'
    uint8_t buf[RLN_BUF_SIZE];
    uint16_t buf_len;
    uint16_t inflight_len;
    // Sticky across rln_read_line; set when a legitimate (drop_cpr=false)
    // CPR is dispatched. Persists across rln_stop / rln_break / rln_init —
    // terminal responsiveness is a property of the peer connection, not of
    // any one line. Used to hold a typed CR through the next handshake
    // instead of tripping typeahead-relief.
    bool seen_cpr;
    // Reset by rln_ansi_reset_for_new_line; set whenever a CPR is
    // dispatched in the current call. Used by rln_clear_stale_seen_cpr
    // so a parser that was responsive previously but silent this round
    // doesn't keep its sticky bit and suppress relief on the next call.
    bool cpr_this_call;
} rln_ansi_t;

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
static absolute_time_t rln_timer;
static uint8_t rln_buflen;
static uint8_t rln_bufpos;
static bool rln_enable_history;
static uint8_t rln_max_length;
static uint32_t rln_timeout_ms;
static uint8_t rln_caps;

// Cross-terminal display state
static rln_phase_t rln_phase;
// Set when rln_enter_edit was called but blocked by the mid-sequence
// safety guard. Cleared when rln_enter_edit successfully enters edit
// phase. rln_ansi_feed retries the entry at the end of every byte
// processed (CAN-reset or dispatch landing back at C0), so whichever
// byte completes the last partial sequence drives the retry.
static bool rln_pending_edit;
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

// One parser instance per input stream. kbd, uart, and tel are fed
// from rln_task as bytes arrive on their respective RX sources; poke
// is fed synchronously from rln_poke (6502 API). Each tracks its
// own stream so a partial CSI sequence on one source can't be sliced
// by interleaved bytes from another.
static rln_ansi_t rln_ansi_kbd;
static rln_ansi_t rln_ansi_uart;
static rln_ansi_t rln_ansi_tel;
static rln_ansi_t rln_ansi_poke;

// False = drop CPR matches arriving on the telnet stream instead of
// dispatching them as geometry. Set by com.c at auth-success based on
// the client's reported TTYPE (script clients get false).
static bool rln_tel_console = true;

// Lastkey capture for the 6502 API. rln_action_taken is the live
// "this dispatch mutated state" flag, sampled at lastkey publish time.
// Typed and poked dispatches both populate these; CPR/DA/DA2 protocol
// replies don't touch lastkey.
static uint8_t rln_lastkey_buf[RLN_LASTKEY_MAX];
static uint8_t rln_lastkey_len;
static bool rln_action_taken;
static bool rln_lastkey_action;

static void rln_complete(bool rln_timed_out)
{
    rln_read_callback_t cc = rln_callback;
    if (!cc)
        return;
    rln_timeout_ms = 0;
    rln_callback = NULL;
    cc(rln_timed_out, rln_buf);
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

/* ----- ICH/DCH emit helpers ----- */

// Emit the screen update for an insertion of `count` chars at buffer
// position `at`. The buffer must already contain the new chars at
// rln_buf[at..at+count-1]. Leaves cursor at rln_bufpos.
//
// No-wrap mode rewrites the tail in place; ICH would shift form data
// to the right of the input. Wrap mode falls back to rln_render_from,
// since a faithful per-row ICH ripple would have to track each row's
// boundary character and resolve xenl at row width — the rewrite is
// simpler and bounded by RLN_MAX_ROWS.
static void rln_emit_insert(uint8_t at, uint8_t count)
{
    if (rln_phase != rln_phase_edit || !count)
        return;
    if (!rln_input_no_wrap())
    {
        rln_render_from(at);
        return;
    }
    rln_sync_cursor_to(at);
    for (uint8_t i = at; i < rln_buflen; i++)
        putchar(rln_buf[i]);
    rln_cur_idx = rln_buflen;
    rln_rendered_end = rln_buflen;
    rln_sync_cursor_to(rln_bufpos);
}

// Emit the screen update for a deletion of `count` chars starting at
// buffer position `at`. The buffer must already reflect the deletion.
// Leaves cursor at rln_bufpos.
//
// No-wrap mode rewrites the tail and space-fills the gap; DCH would
// pull form data to the right of the input. Wrap mode falls back to
// rln_render_from for the same reason as rln_emit_insert.
static void rln_emit_delete(uint8_t at, uint8_t count)
{
    if (rln_phase != rln_phase_edit || !count)
        return;
    if (!rln_input_no_wrap())
    {
        rln_render_from(at);
        return;
    }
    rln_sync_cursor_to(at);
    for (uint8_t i = at; i < rln_buflen; i++)
        putchar(rln_buf[i]);
    for (uint8_t i = 0; i < count; i++)
        putchar(' ');
    printf("\33[%uD", count);
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
    if (!rln_enable_history || rln_timeout_ms)
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
    if (!rln_enable_history || rln_timeout_ms)
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

// Park the cursor past the input, emit a newline, nul-terminate the
// buffer, optionally push to history, and complete. Used by both the
// typed CR dispatch and rln_poke's control-byte completion path.
static void rln_finish_line(bool add_to_history)
{
    rln_sync_cursor_to(rln_buflen);
    printf("\n");
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

static void rln_line_forward(rln_ansi_t *a)
{
    if (a->csi_param_count > 1 && a->csi_param[1] != 1)
        return rln_line_word(1);
    int count = a->csi_param[0];
    if (count < 1)
        count = 1;
    rln_step(count);
}

static void rln_line_backward(rln_ansi_t *a)
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
    rln_emit_delete(start, count);
}

static void rln_line_delete(void)
{
    rln_delete_range(rln_bufpos, rln_bufpos + 1);
}

static void rln_line_delete_n(rln_ansi_t *a)
{
    uint16_t count = a->csi_param[0];
    if (count < 1)
        count = 1;
    uint16_t end = (uint16_t)rln_bufpos + count;
    if (end > UINT8_MAX)
        end = UINT8_MAX;
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
static void rln_line_insert_n(rln_ansi_t *a)
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
    rln_emit_insert(rln_bufpos, (uint8_t)count);
}

// ECH (CSI Ps X): erase Ps chars at the cursor by replacing them with
// spaces. Cursor stays put, buflen unchanged, no shift. Stops at buflen.
static void rln_line_erase_n(rln_ansi_t *a)
{
    uint16_t count = a->csi_param[0];
    if (count < 1)
        count = 1;
    uint16_t end = (uint16_t)rln_bufpos + count;
    if (end > rln_buflen)
        end = rln_buflen;
    if (end <= rln_bufpos)
        return;
    uint8_t at = rln_bufpos;
    uint8_t span = (uint8_t)(end - at);
    memset(rln_buf + at, ' ', span);
    rln_action_taken = true;
    // Tail length unchanged; rln_emit_insert's redraw path is exactly
    // what we need — sync to `at`, rewrite the tail, restore cursor.
    rln_emit_insert(at, span);
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
                printf("\n");
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
    rln_emit_insert(at, 1);
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

// Forward decl needed to break the call cycle: rln_cpr_dispatch ->
// rln_enter_edit -> rln_ansi_drain_deferred -> rln_ansi_feed ->
// rln_ansi_dispatch_or_defer -> rln_cpr_dispatch.
static void rln_cpr_dispatch(uint16_t p1, uint16_t p2);
// rln_ansi_feed reruns the deferred entry from its own tail when the
// mid-sequence guard previously blocked rln_enter_edit.
static void rln_enter_edit(void);

static void rln_ansi_advance(rln_ansi_t *a, uint8_t ch)
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

static void rln_dispatch_C0(rln_ansi_t *a, uint8_t ch)
{
    (void)a;
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

static void rln_dispatch_Fe(rln_ansi_t *a, uint8_t ch)
{
    (void)a;
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

static void rln_dispatch_SS3(rln_ansi_t *a, uint8_t ch)
{
    (void)a;
    if (ch == 'F')
        rln_line_end();
    else if (ch == 'H')
        rln_line_home();
}

static void rln_dispatch_CSI(rln_ansi_t *a, uint8_t ch)
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
static void rln_ansi_trim_inflight(rln_ansi_t *a)
{
    a->buf_len -= a->inflight_len;
    a->inflight_len = 0;
}

// Snapshot the in-flight tail into rln_lastkey_buf. Called only from
// typed-action dispatch during edit phase.
static void rln_publish_lastkey(rln_ansi_t *a)
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
static void rln_da2_dispatch(void)
{
    if (rln_decscusr_ok)
        return;
    rln_decscusr_ok = true;
    if (rln_phase == rln_phase_edit)
        rln_emit_mode_cursor();
}

// Classify the just-completed sequence and dispatch or defer.
// entry_state is the parser's state BEFORE this byte was advanced;
// term is this byte (the terminator).
static void rln_ansi_dispatch_or_defer(rln_ansi_t *a,
                                       uint8_t term,
                                       rln_ansi_state_t entry_state,
                                       bool drop_cpr)
{
    // CPR: ESC [ p1 ; p2 R, no private marker, exactly 2 params.
    if (entry_state == ansi_state_CSI &&
        term == 'R' &&
        a->csi_private == 0 &&
        a->csi_param_count == 2)
    {
        if (!drop_cpr)
        {
            a->seen_cpr = true;
            a->cpr_this_call = true;
            rln_cpr_dispatch(a->csi_param[0], a->csi_param[1]);
        }
        rln_ansi_trim_inflight(a);
        return;
    }
    // DA2: ESC [ > ... c. Param count and values are irrelevant.
    if (entry_state == ansi_state_CSI_private &&
        a->csi_private == '>' &&
        term == 'c')
    {
        rln_da2_dispatch();
        rln_ansi_trim_inflight(a);
        return;
    }
    // CSI_private with any other terminator: silent no-op (today's
    // behaviour). Trim and continue.
    if (entry_state == ansi_state_CSI_private)
    {
        rln_ansi_trim_inflight(a);
        return;
    }
    // SS2: no-op (today's behaviour).
    if (entry_state == ansi_state_SS2)
    {
        rln_ansi_trim_inflight(a);
        return;
    }
    // Typed action — dispatch in edit phase, defer in handshake.
    if (rln_phase != rln_phase_edit)
    {
        // Defer: leave bytes in buf head, just reset inflight so the
        // next byte starts a fresh sequence.
        a->inflight_len = 0;
        return;
    }
    rln_action_taken = false;
    switch (entry_state)
    {
    case ansi_state_C0:
        rln_dispatch_C0(a, term);
        break;
    case ansi_state_Fe:
        rln_dispatch_Fe(a, term);
        break;
    case ansi_state_SS3:
        rln_dispatch_SS3(a, term);
        break;
    case ansi_state_CSI:
        rln_dispatch_CSI(a, term);
        break;
    default:
        break;
    }
    rln_publish_lastkey(a);
    rln_ansi_trim_inflight(a);
}

// Feed one byte through a per-stream parser. drop_cpr=true on the
// telnet parser when the client is dumb so CPR matches are consumed
// silently instead of pinning geometry to junk values; also true on
// the poke parser so accidentally-poked CPR shapes can't pin geometry.
static void rln_ansi_feed(rln_ansi_t *a, uint8_t b, bool drop_cpr)
{
    // CAN aborts any in-progress sequence with no dispatch.
    if (b == '\30')
    {
        rln_ansi_trim_inflight(a);
        a->state = ansi_state_C0;
    }
    // Append to in-flight cache. On overflow drop the byte — instant
    // relief in rln_task will trigger from the buf_len == cap check.
    else if (a->buf_len < sizeof a->buf)
    {
        a->buf[a->buf_len++] = b;
        a->inflight_len++;
        rln_ansi_state_t entry_state = a->state;
        rln_ansi_advance(a, b);
        if (a->state == ansi_state_C0)
            rln_ansi_dispatch_or_defer(a, b, entry_state, drop_cpr);
    }
    // If a prior rln_enter_edit was deferred by the mid-sequence guard,
    // retry now. Either this byte returned a parser to C0 (CAN reset or
    // dispatch landed) or it left this parser mid-sequence; rln_enter_edit
    // re-checks all four parsers and either enters or stays pending.
    if (rln_pending_edit)
        rln_enter_edit();
}

// Drain the deferred head region through the same parser. Called from
// rln_enter_edit. Re-feeds every byte currently in buf[]: protocol
// replies were trimmed at dispatch time during handshake, so what's
// left is typed bytes + any in-flight tail. Replaying preserves any
// in-flight state. Snapshots rln_callback at entry so that a CR in
// temp[] which completes the line (and may even synchronously start
// a new one) doesn't replay further stale bytes against the next
// line's freshly memset state.
static void rln_ansi_drain_deferred(rln_ansi_t *a, bool drop_cpr)
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
        rln_ansi_feed(a, temp[i], drop_cpr);
        if (rln_callback != cb_at_start)
            return;
    }
}

/* ----- Phase transitions ----- */

// True iff any per-stream parser is mid-escape-sequence (state != C0).
// kbd/uart/tel are fed by rln_task; poke is fed by rln_poke. Any of
// them being mid-sequence means a dispatch is imminent — we should wait
// for it rather than draining a partial sequence whose tail would be
// silently re-fed but whose dispatch result (geometry, mode flips) we'd
// then race against the late-CPR guard in rln_cpr_dispatch.
static bool rln_any_parser_in_sequence(void)
{
    return rln_ansi_kbd.state != ansi_state_C0 ||
           rln_ansi_uart.state != ansi_state_C0 ||
           rln_ansi_tel.state != ansi_state_C0 ||
           rln_ansi_poke.state != ansi_state_C0;
}

static void rln_enter_edit(void)
{
    // Safety: don't enter edit while any parser is mid-escape-sequence.
    // The byte that completes the sequence will run rln_ansi_feed's tail
    // retry and we'll come back here once everyone is at C0. Note this
    // is purely a structural guard — it doesn't decide *whether* we
    // should be in edit phase (the call sites own that), only that the
    // transition itself has to wait for byte-level quiescence.
    if (rln_any_parser_in_sequence())
    {
        rln_pending_edit = true;
        return;
    }
    rln_pending_edit = false;
    rln_phase = rln_phase_edit;
    rln_emit_mode_cursor();
    rln_cur_idx = 0;
    if (rln_buflen)
        rln_render_from(0);
    else
        rln_sync_cursor_to(rln_bufpos);
    // Drain each parser's deferred bytes through itself, in order:
    // kbd (local human) → uart (local terminal) → tel (remote) →
    // poke (scripted input). drop_cpr=true on kbd because the local
    // keyboard never legitimately produces protocol replies; same
    // rationale as poke. Abort the cascade if a dispatched CR mid-
    // drain completes the line — any later drains would replay
    // stale bytes into the next line's parsers.
    rln_read_callback_t cb_at_start = rln_callback;
    rln_ansi_drain_deferred(&rln_ansi_kbd, /*drop_cpr=*/true);
    if (rln_callback != cb_at_start)
        return;
    rln_ansi_drain_deferred(&rln_ansi_uart, /*drop_cpr=*/false);
    if (rln_callback != cb_at_start)
        return;
    rln_ansi_drain_deferred(&rln_ansi_tel, !rln_tel_console);
    if (rln_callback != cb_at_start)
        return;
    bool prev_overwrite = rln_overwrite;
    rln_overwrite = true;
    rln_ansi_drain_deferred(&rln_ansi_poke, /*drop_cpr=*/true);
    bool need_reemit = (rln_overwrite != prev_overwrite);
    rln_overwrite = prev_overwrite;
    if (need_reemit)
        rln_emit_mode_cursor();
}

// Per-line parser reset. Preserves seen_cpr (terminal responsiveness is
// a property of the peer connection, not of any one rln_read_line call);
// everything else — including cpr_this_call — gets zeroed in the common
// case.
//
// Special case: mid-escape-sequence at line boundary. When poke (or any
// other path that bypasses dispatch_C0) calls rln_finish_line, the
// callback can synchronously start a new rln_read_line while another
// stream's parser is mid-sequence — e.g., kbd holds an arrow key and
// rln_ansi_kbd is sitting on `state=Fe, buf=[\33]` waiting for the `[A`.
// Wiping that here would turn the trailing `[A` into a literal `A`
// dispatched in the new line. Preserve state/csi_*/buf/inflight_len in
// that case so the bytes still in flight land correctly in the new line.
static void rln_ansi_reset_for_new_line(rln_ansi_t *a)
{
    if (a->state != ansi_state_C0 || a->inflight_len > 0)
    {
        // Mid-sequence carryover. Drop only per-call CPR tracking.
        a->cpr_this_call = false;
        return;
    }
    bool seen = a->seen_cpr;
    memset(a, 0, sizeof *a);
    a->seen_cpr = seen;
}

// Clear seen_cpr on any parser that didn't dispatch a CPR this call.
// Called from any path that enters edit phase without completing the CPR
// sequence: rln_handshake_fallback (deadline) and the typeahead-relief
// branch in rln_task. A parser that was responsive in a prior call but
// silent this round must not keep its sticky bit, or the next call's
// relief will stay suppressed for a peer that is no longer answering.
static void rln_clear_stale_seen_cpr(void)
{
    if (!rln_ansi_uart.cpr_this_call)
        rln_ansi_uart.seen_cpr = false;
    if (!rln_ansi_tel.cpr_this_call)
        rln_ansi_tel.seen_cpr = false;
}

// True while we are pre-edit and uart or tel has dispatched any CPR
// (this call or sticky from a prior call). The peer is known responsive
// and the rest of the CPR1/DA2/CPR2 burst is on its way; any path that
// would complete or terminate the line synchronously must instead defer
// the triggering byte through the per-stream parser so the drain in
// rln_enter_edit can dispatch it after edit phase is reached. Returns
// false in edit phase so callers don't need a separate phase guard.
static bool rln_expecting_more_cpr(void)
{
    return rln_phase != rln_phase_edit &&
           (rln_ansi_uart.seen_cpr || rln_ansi_tel.seen_cpr);
}

static void rln_handshake_fallback(void)
{
    // Handshake deadline expired. The init burst already RCP'd the
    // cursor back to the prompt and re-showed it, so we don't need
    // to fix cursor placement here. We never got both CPRs (CPR2
    // would have advanced us straight to edit), so drop into the
    // single-virtual-line model (row math bypassed by prompt_col==0)
    // and preserve whatever DA2 told us about DECSCUSR.
    rln_clear_stale_seen_cpr();
    rln_prompt_col = 0;
    rln_term_height = 0;
    rln_term_width = 0;
    rln_enter_edit();
}

// Dispatch a parsed CPR (\33[<p1>;<p2>R). Row is in p1, col in p2.
// Content-based routing classifies replies across multiple terminals:
//   - First reply ever: prompt CPR.
//   - Next reply with col > prompt_col: size CPR (drives transition).
//   - Later replies whose col differs from prompt_col: clamp width down.
//   - Anything else (incl. CPR2 echoing prompt_col when the terminal
//     ignored \33[999;999H): discard, let the deadline fall through
//     to single-line fallback rather than wrap at the prompt column.
static void rln_cpr_dispatch(uint16_t p1, uint16_t p2)
{
    bool both_overrides = rln_width_override && rln_height_override;
    if (rln_prompt_col == 0)
    {
        rln_prompt_col = p2 ? p2 : 1;
        if (both_overrides)
            rln_enter_edit();
        else
            rln_phase = rln_phase_width_cpr;
    }
    else if (both_overrides)
    {
        // Discard everything after CPR1 when geometry is fully pinned.
        return;
    }
    else if (rln_term_height == 0)
    {
        if (p2 <= rln_prompt_col)
            return; // bogus size; keep waiting for a useful CPR
        rln_term_height = p1;
        rln_term_width = p2;
        rln_enter_edit();
    }
    else if (p2 > rln_prompt_col)
    {
        // Late size CPR. Either the first one to arrive after typeahead-
        // relief preempted CPR2 (rln_term_width == 0), or a narrower/
        // shorter shadow terminal. Track the running minimum for both axes.
        bool width_changed = false;
        if (rln_term_width == 0 || p2 < rln_term_width)
        {
            rln_term_width = p2;
            width_changed = true;
        }
        if (rln_term_height == 0 || p1 < rln_term_height)
            rln_term_height = p1;
        if (width_changed && rln_phase == rln_phase_edit)
            rln_render_from(0);
    }
}

/* ----- Top-level task / lifecycle ----- */

void rln_read_line(rln_read_callback_t callback)
{
    rln_timeout_ms = 0;
    rln_buflen = 0;
    rln_bufpos = 0;
    rln_callback = callback;
    rln_history_pos = 0;
    rln_buf[0] = 0;
    rln_lastkey_len = 0;
    rln_action_taken = false;
    rln_lastkey_action = false;

    rln_phase = rln_phase_prompt_cpr;
    rln_pending_edit = false;
    // Sentinels for the content-based CPR heuristic: 0 means
    // "not pinned yet". Must be cleared every call or replies
    // from the next handshake get misclassified.
    rln_prompt_col = 0;
    rln_term_height = 0;
    rln_term_width = 0;
    rln_cur_idx = 0;
    rln_rendered_max_row = 0;
    rln_rendered_end = 0;
    rln_ansi_reset_for_new_line(&rln_ansi_kbd);
    rln_ansi_reset_for_new_line(&rln_ansi_uart);
    rln_ansi_reset_for_new_line(&rln_ansi_tel);
    rln_ansi_reset_for_new_line(&rln_ansi_poke);
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
    {
        // Re-probing DA2; clear the cached availability so a silent
        // (non-VT220) peer leaves it false. max_length==0 lines skip
        // the probe — they own no column to overwrite a minicom-style
        // `c` leak — and retain whatever an earlier line cached.
        rln_decscusr_ok = false;
        printf("\33[>c\33[u \b");
    }
    if (!(rln_width_override && rln_height_override))
        printf("\33[999;999H\33[6n\33[u");
    printf("\33[?25h");
}

void rln_read_line_timeout(rln_read_callback_t callback, uint32_t timeout_ms)
{
    assert(timeout_ms);
    rln_read_line(callback);
    rln_timeout_ms = timeout_ms;
    rln_timer = make_timeout_time_ms(rln_timeout_ms);
}

void rln_task(void)
{
    if (!rln_callback)
        return;
    // Pull bytes via com_getchar, which respects any active script
    // hold (held source delivers; others drain to /dev/null) and
    // otherwise walks its sticky kbd/UART/telnet multiplex. The
    // reported source identity drives parser selection and instant-
    // relief tagging. drop_cpr=true on kbd because a local keyboard
    // never legitimately produces protocol replies.
    while (rln_callback)
    {
        com_source_t this_src;
        int c = com_getchar(&this_src);
        if (c < 0)
            break;
        char ch = (char)c;
        rln_timer = make_timeout_time_ms(rln_timeout_ms);
        switch (this_src)
        {
        case COM_SOURCE_TEL:
            rln_ansi_feed(&rln_ansi_tel, (uint8_t)ch, !rln_tel_console);
            break;
        case COM_SOURCE_UART:
            rln_ansi_feed(&rln_ansi_uart, (uint8_t)ch, /*drop_cpr=*/false);
            break;
        case COM_SOURCE_KBD:
            rln_ansi_feed(&rln_ansi_kbd, (uint8_t)ch, /*drop_cpr=*/true);
            break;
        case COM_SOURCE_NONE:
            break;
        }
        // Provide instant relief during handshake for scripting tools
        // that do not respond to ANSI sequences. The triggering source
        // is identified as a script — arm the com-level hold so any
        // late CPR/DA2 from other sources is swallowed before reaching
        // edit phase. Only fires pre-edit; once we enter edit phase
        // subsequent iterations skip this check.
        //
        // The CR trigger is suppressed while rln_expecting_more_cpr() —
        // a responsive peer's burst is still landing, so we hold the CR
        // in the parser's buf[] and let rln_enter_edit's drain dispatch
        // it once the sequence finishes (or the deadline expires).
        // Buffer-overflow still triggers unconditionally as a safety net.
        if (rln_phase != rln_phase_edit &&
            ((ch == '\r' && !rln_expecting_more_cpr()) ||
             rln_ansi_kbd.buf_len >= RLN_BUF_SIZE ||
             rln_ansi_uart.buf_len >= RLN_BUF_SIZE ||
             rln_ansi_tel.buf_len >= RLN_BUF_SIZE))
        {
            // Skip the rest of the CPR handshake. We never confirmed
            // geometry for this session, so drop prompt_col to force
            // no-wrap rendering (no row math at unverified widths).
            // rln_term_width is left as-is — a value captured by a
            // previous successful handshake is still useful for callers
            // that query rln_get_term_width(); it's only zeroed on a
            // real handshake timeout in rln_handshake_fallback().
            rln_clear_stale_seen_cpr();
            com_getchar_source(this_src);
            rln_prompt_col = 0;
            rln_enter_edit();
        }
    }
    if (rln_phase != rln_phase_edit && rln_callback &&
        time_reached(rln_handshake_deadline))
        rln_handshake_fallback();
    if (rln_timeout_ms && time_reached(rln_timer))
        rln_complete(true);
}

void rln_init(void)
{
    rln_callback = NULL;
    rln_enable_history = true;
    rln_max_length = 255; // fits in 2^8 with nul
    rln_caps = RLN_CAPS_OFF;
    rln_phase = rln_phase_edit;
    rln_overwrite = false;
    rln_width_override = 0;
    rln_height_override = 0;
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
    printf("\n");
    rln_init();
}

/* 6502 applications may configure the max length */

void rln_set_max_length(uint8_t v) { rln_max_length = v; }
uint8_t rln_get_max_length(void) { return rln_max_length; }

void rln_set_caps(uint8_t v) { rln_caps = v; }
uint8_t rln_get_caps(void) { return rln_caps; }

void rln_set_tel_console(bool active)
{
    rln_tel_console = active;
    memset(&rln_ansi_tel, 0, sizeof rln_ansi_tel);
}

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

// Any C0 control byte (0x00-0x1F) in the poked stream finishes the
// input, except for ESC (\33) which starts a CSI sequence and CAN
// (\30) which the parser uses to abort an in-flight sequence and
// reset state. As a visual badge, controls other than CR (\r) and
// LF (\n) echo as caret notation (^@..^_).
//
// While rln_expecting_more_cpr() — a responsive peer is mid-CPR-burst —
// the immediate-finish path is skipped and the byte is queued through
// the parser instead. Drain at edit phase will route a CR through
// dispatch_C0 to rln_finish_line; other control bytes are dropped
// (rare in poked input during the brief handshake window).
void rln_poke(const char *str)
{
    if (!rln_callback)
        return;
    bool sync = (rln_phase == rln_phase_edit);
    bool prev_overwrite = rln_overwrite;
    if (sync)
        rln_overwrite = true;
    for (const char *p = str; *p; p++)
    {
        uint8_t ch = (uint8_t)*p;
        if (ch < 32 && ch != '\33' && ch != '\30' &&
            !rln_expecting_more_cpr())
        {
            if (ch != '\r' && ch != '\n' && rln_max_length >= 2)
            {
                uint8_t target = rln_bufpos;
                if (target > (uint8_t)(rln_max_length - 2))
                    target = (uint8_t)(rln_max_length - 2);
                rln_sync_cursor_to(target);
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
                            printf("\n");
                            c = 1;
                        }
                        else
                            c++;
                    }
                }
                rln_cur_idx = (uint8_t)(target + 2);
            }
            rln_finish_line(ch == '\r');
            break;
        }
        rln_ansi_feed(&rln_ansi_poke, (uint8_t)ch, /*drop_cpr=*/true);
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
