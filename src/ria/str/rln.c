/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "sys/ria.h"
#include "sys/vga.h"
#include "str/rln.h"
#include "str/str.h"
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
    rln_phase_width_cpr,  // waiting for second CPR (height + width); the
                          // burst order guarantees DA2's reply lands first,
                          // but DA2 is absorbed by the always-on CPR sniffer
                          // regardless of phase, so this phase just gates
                          // the CPR-driven transition to edit
    rln_phase_edit,       // normal editing
} rln_phase_t;

#define RLN_BUF_SIZE 256
#define RLN_HISTORY_SIZE 8
#define RLN_CSI_PARAM_MAX_LEN 16
#define RLN_LASTKEY_MAX 32
#define RLN_TYPEAHEAD_MAX 32
#define RLN_CPR_SEQ_MAX 12
#define RLN_HANDSHAKE_MS 500
#define RLN_MAX_ROWS 10

typedef struct
{
    rln_ansi_state_t state;
    uint16_t csi_param[RLN_CSI_PARAM_MAX_LEN];
    uint8_t csi_param_count;
} rln_ansi_t;

// History storage
// history[0] is the current input being edited.
// history[1..RLN_HISTORY_SIZE-1] hold previous entries.
static char rln_history[RLN_HISTORY_SIZE][RLN_BUF_SIZE];
static uint8_t rln_history_count;
static uint8_t rln_history_pos;

// Input state
static char rln_buf[RLN_BUF_SIZE];
static rln_read_callback_t rln_callback;
static absolute_time_t rln_timer;
static uint8_t rln_buflen;
static uint8_t rln_bufpos;
static rln_ansi_t rln_typed_ansi;
static bool rln_enable_history;
static uint8_t rln_max_length;
static uint32_t rln_timeout_ms;
static uint8_t rln_caps;

// Cross-terminal display state
static rln_phase_t rln_phase;
static absolute_time_t rln_handshake_deadline;
static uint8_t rln_prompt_col;   // 1-based
static uint16_t rln_term_width;  // 0 = fallback (single virtual line)
static uint16_t rln_term_height; // 0 in fallback; exposed for mon.c
static uint8_t rln_cur_idx;      // buffer index whose screen position the cursor is at
static bool rln_overwrite;       // persisted across rln_read_line calls
static bool rln_decscusr_ok;     // peer claimed VT220+ via Primary DA; safe to emit DECSCUSR
static uint8_t rln_rendered_max_row; // highest row index rln has written to in the current line

// CPR mini-parser state (active only during handshake phases)
static uint8_t rln_cpr_state;
static uint16_t rln_cpr_p1;
static uint16_t rln_cpr_p2;
static uint8_t rln_cpr_seq[RLN_CPR_SEQ_MAX];
static uint8_t rln_cpr_seq_len;

// Typeahead ring: bytes received during the CPR handshake that weren't part
// of a CPR reply. Drained through the normal parser once phase = edit.
static uint8_t rln_typeahead[RLN_TYPEAHEAD_MAX];
static uint8_t rln_typeahead_len;

// Lastkey capture (typed stream only)
static uint8_t rln_lastkey_buf[RLN_LASTKEY_MAX];
static uint8_t rln_lastkey_len;
static uint8_t rln_keyseq_accum[RLN_LASTKEY_MAX];
static uint8_t rln_keyseq_accum_len;
static bool rln_keyseq_overflow;

static void rln_complete(bool rln_timed_out)
{
    rln_read_callback_t cc = rln_callback;
    rln_timeout_ms = 0;
    rln_callback = NULL;
    cc(rln_timed_out, rln_buf);
}

/* ----- Screen position math (multi-line mode) ----- */

static void rln_buf_to_screen(uint8_t i, uint8_t *row, uint16_t *col)
{
    uint32_t logical = (uint32_t)(rln_prompt_col - 1) + i;
    *row = (uint8_t)(logical / rln_term_width);
    *col = (uint16_t)(logical % rln_term_width) + 1;
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
    if (rln_term_width == 0)
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
// rln_bufpos. Stale content is wiped per-row with \33[K — never
// \33[J, which would erase rows below that rln does not own.
static void rln_render_from(uint8_t start)
{
    if (rln_phase != rln_phase_edit)
        return;
    rln_sync_cursor_to(start);
    if (rln_term_width == 0)
    {
        printf("\33[K");
        for (uint8_t i = start; i < rln_buflen; i++)
            putchar(rln_buf[i]);
        rln_cur_idx = rln_buflen;
    }
    else
    {
        // Clear the start row from the cursor onward, then write
        // chars. Each forced wrap clears the new row before writing
        // into it, so the only rows we ever touch are rln's own.
        uint8_t r;
        uint16_t c;
        rln_buf_to_screen(start, &r, &c);
        printf("\33[K");
        for (uint8_t i = start; i < rln_buflen; i++)
        {
            putchar(rln_buf[i]);
            if (c == rln_term_width)
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
    }
    rln_sync_cursor_to(rln_bufpos);
}

/* ----- ICH/DCH emit helpers ----- */

// Emit the screen update for an insertion of `count` chars at buffer
// position `at`. The buffer must already contain the new chars at
// rln_buf[at..at+count-1]. Leaves cursor at rln_bufpos.
//
// Single-line mode (term_width == 0) uses ICH (CSI Ps @) so the
// terminal shifts the tail itself; only the new chars travel the
// wire. Multi-line mode falls back to a tail rewrite because a
// faithful per-row ICH ripple has to track each row's boundary
// character and resolve xenl at row width — the rewrite is simpler
// and the size difference is bounded by RLN_MAX_ROWS.
static void rln_emit_insert(uint8_t at, uint8_t count)
{
    if (rln_phase != rln_phase_edit || !count)
        return;
    if (rln_term_width)
    {
        rln_render_from(at);
        return;
    }
    rln_sync_cursor_to(at);
    printf("\33[%u@", count);
    for (uint8_t i = 0; i < count; i++)
        putchar(rln_buf[at + i]);
    rln_cur_idx = (uint8_t)(at + count);
    rln_sync_cursor_to(rln_bufpos);
}

// Emit the screen update for a deletion of `count` chars starting at
// buffer position `at`. The buffer must already reflect the deletion.
// Leaves cursor at rln_bufpos.
//
// Single-line uses DCH (CSI Ps P). Multi-line falls back to
// rln_render_from for the same reason as rln_emit_insert.
static void rln_emit_delete(uint8_t at, uint8_t count)
{
    if (rln_phase != rln_phase_edit || !count)
        return;
    if (rln_term_width)
    {
        rln_render_from(at);
        return;
    }
    rln_sync_cursor_to(at);
    printf("\33[%uP", count);
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

/* ----- Movement helpers ----- */

static void rln_line_home(void)
{
    rln_bufpos = 0;
    rln_sync_cursor_to(rln_bufpos);
}

static void rln_line_end(void)
{
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

static void rln_line_forward_word(void)
{
    uint8_t to = rln_scan_word(1);
    if (to == rln_bufpos)
        return;
    rln_bufpos = to;
    rln_sync_cursor_to(rln_bufpos);
}

static void rln_line_backward_word(void)
{
    uint8_t to = rln_scan_word(-1);
    if (to == rln_bufpos)
        return;
    rln_bufpos = to;
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
        rln_sync_cursor_to(rln_bufpos);
}

static void rln_line_forward(rln_ansi_t *a)
{
    if (a->csi_param_count > 1 && a->csi_param[1] != 1)
        return rln_line_forward_word();
    int count = a->csi_param[0];
    if (count < 1)
        count = 1;
    rln_step(count);
}

static void rln_line_backward(rln_ansi_t *a)
{
    if (a->csi_param_count > 1 && a->csi_param[1] != 1)
        return rln_line_backward_word();
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
    if (end > 255)
        end = 255;
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
    if (rln_term_width)
    {
        uint16_t avail = (uint16_t)RLN_MAX_ROWS * rln_term_width;
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

static void rln_line_insert(char ch)
{
    if ((unsigned char)ch < 32)
        return;
    if (rln_caps == 1 && islower((unsigned char)ch))
        ch = toupper((unsigned char)ch);
    else if (rln_caps == 2)
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
        if (rln_phase == rln_phase_edit)
        {
            rln_sync_cursor_to(rln_bufpos);
            bool at_row_end = false;
            if (rln_term_width)
            {
                uint8_t r;
                uint16_t c;
                rln_buf_to_screen(rln_bufpos, &r, &c);
                at_row_end = (c == rln_term_width);
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
    rln_emit_insert(at, 1);
}

/* ----- Mode toggling ----- */

// DECSCUSR (CSI Ps SP q) is only emitted when the peer has identified
// itself as VT220+ via Primary DA. VT102 emulators (minicom, Linux
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
    if (rln_phase == rln_phase_edit)
        rln_emit_mode_cursor();
}

/* ----- ANSI parser ----- */

static void rln_line_state_C0(rln_ansi_t *a, char ch)
{
    if (ch == '\r')
    {
        rln_sync_cursor_to(rln_buflen);
        printf("\n");
        rln_buf[rln_buflen] = 0;
        rln_history_add();
        rln_complete(false);
    }
    else if (ch == '\33')
        a->state = ansi_state_Fe;
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

static void rln_line_state_Fe(rln_ansi_t *a, char ch)
{
    if (ch == '[')
    {
        a->state = ansi_state_CSI;
        a->csi_param_count = 0;
        a->csi_param[0] = 0;
    }
    else if (ch == 'b' || ch == 2)
    {
        a->state = ansi_state_C0;
        rln_line_backward_word();
    }
    else if (ch == 'f' || ch == 6)
    {
        a->state = ansi_state_C0;
        rln_line_forward_word();
    }
    else if (ch == 'N')
        a->state = ansi_state_SS2;
    else if (ch == 'O')
        a->state = ansi_state_SS3;
    else if (ch == 'd')
    {
        a->state = ansi_state_C0;
        rln_line_forward_kill_word();
    }
    else
    {
        a->state = ansi_state_C0;
        if (ch == 127 || ch == '\b')
            rln_line_backward_kill_word();
    }
}

static void rln_line_state_SS2(rln_ansi_t *a, char ch)
{
    (void)ch;
    a->state = ansi_state_C0;
}

static void rln_line_state_SS3(rln_ansi_t *a, char ch)
{
    a->state = ansi_state_C0;
    if (ch == 'F')
        rln_line_end();
    else if (ch == 'H')
        rln_line_home();
}

static void rln_line_state_CSI(rln_ansi_t *a, char ch)
{
    if (isdigit(ch))
    {
        if (a->csi_param_count < RLN_CSI_PARAM_MAX_LEN)
        {
            a->csi_param[a->csi_param_count] *= 10;
            a->csi_param[a->csi_param_count] += ch - '0';
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
        return;
    }
    if (a->state == ansi_state_CSI_private)
    {
        a->state = ansi_state_C0;
        return;
    }
    a->state = ansi_state_C0;
    a->csi_param_count++;
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
    else if (ch == 'b' || ch == 2)
        rln_line_backward_word();
    else if (ch == 'f' || ch == 6)
        rln_line_forward_word();
    else if (ch == '~')
        switch (a->csi_param[0])
        {
        case 1:
        case 7:
            return rln_line_home();
        case 2:
            return rln_toggle_overwrite();
        case 4:
        case 8:
            return rln_line_end();
        case 3:
            return rln_line_delete();
        }
}

static void rln_line_rx(rln_ansi_t *a, uint8_t ch)
{
    if (ch == '\30')
        a->state = ansi_state_C0;
    else
        switch (a->state)
        {
        case ansi_state_C0:
            rln_line_state_C0(a, ch);
            break;
        case ansi_state_Fe:
            rln_line_state_Fe(a, ch);
            break;
        case ansi_state_SS2:
            rln_line_state_SS2(a, ch);
            break;
        case ansi_state_SS3:
            rln_line_state_SS3(a, ch);
            break;
        case ansi_state_CSI:
        case ansi_state_CSI_private:
            rln_line_state_CSI(a, ch);
            break;
        }
}

static void rln_line_rx_typed(uint8_t ch)
{
    rln_ansi_state_t prev_state = rln_typed_ansi.state;
    if (prev_state == ansi_state_C0)
    {
        rln_keyseq_accum[0] = ch;
        rln_keyseq_accum_len = 1;
        rln_keyseq_overflow = false;
    }
    else if (rln_keyseq_accum_len < RLN_LASTKEY_MAX)
        rln_keyseq_accum[rln_keyseq_accum_len++] = ch;
    else
        rln_keyseq_overflow = true;

    rln_line_rx(&rln_typed_ansi, ch);

    if (rln_typed_ansi.state == ansi_state_C0)
    {
        if (!rln_keyseq_overflow)
        {
            memcpy(rln_lastkey_buf, rln_keyseq_accum, rln_keyseq_accum_len);
            rln_lastkey_len = rln_keyseq_accum_len;
        }
        rln_keyseq_overflow = false;
    }
}

/* ----- CPR handshake (mini-parser + phase transitions) ----- */

static void rln_typeahead_push(uint8_t b)
{
    if (rln_phase == rln_phase_edit)
    {
        // Sniffer is always-on, so non-CPR bytes that arrive in
        // edit phase need to flow straight to the typed parser
        // instead of buffering for a drain that already happened.
        rln_line_rx_typed(b);
        return;
    }
    if (rln_typeahead_len < RLN_TYPEAHEAD_MAX)
        rln_typeahead[rln_typeahead_len++] = b;
    // overflow silently dropped — handshake window is short
}

static void rln_cpr_release_seq(void)
{
    for (uint8_t i = 0; i < rln_cpr_seq_len; i++)
        rln_typeahead_push(rln_cpr_seq[i]);
    rln_cpr_seq_len = 0;
    rln_cpr_state = 0;
}

static void rln_typeahead_drain(void)
{
    uint8_t len = rln_typeahead_len;
    rln_typeahead_len = 0;
    for (uint8_t i = 0; i < len; i++)
        rln_line_rx_typed(rln_typeahead[i]);
}

static void rln_enter_edit(void)
{
    rln_phase = rln_phase_edit;
    rln_emit_mode_cursor();
    rln_cur_idx = 0;
    if (rln_buflen)
        rln_render_from(0);
    else
        rln_sync_cursor_to(rln_bufpos);
    rln_typeahead_drain();
}

static void rln_handshake_fallback(void)
{
    // Handshake deadline expired. The init burst already RCP'd the
    // cursor back to the prompt and re-showed it, so we don't need
    // to fix cursor placement here. We never got both CPRs (CPR2
    // would have advanced us straight to edit), so drop into the
    // single-virtual-line model (row math bypassed by term_width==0)
    // and preserve whatever DA2 told us about DECSCUSR.
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
    if (rln_prompt_col == 0)
    {
        rln_prompt_col = p2 ? (p2 > 255 ? 255 : (uint8_t)p2) : 1;
        rln_phase = rln_phase_width_cpr;
    }
    else if (rln_term_height == 0)
    {
        if (p2 <= rln_prompt_col)
            return; // bogus size; keep waiting for a useful CPR
        rln_term_height = p1;
        rln_term_width = p2;
        rln_enter_edit();
    }
    else if (p2 > rln_prompt_col && p2 < rln_term_width)
    {
        // Late size CPR from a narrower shadow terminal. Clamp the
        // working width down and re-flow the visible buffer so the
        // wraps land at the same logical column on every display.
        rln_term_width = p2;
        if (rln_phase == rln_phase_edit)
            rln_render_from(0);
    }
}

// Dispatch a parsed Secondary DA reply (\33[><...>c). The fact that
// a reply arrived at all is the signal — minicom and similarly broken
// peers ignore DA2 (and leak its `c` as literal text, scrubbed by the
// burst's DECRC+EL). Param values are discarded. A late DA2 arriving
// in edit phase still flips the bit and emits the cursor shape so the
// user sees the correct insert/overwrite indicator going forward.
static void rln_da2_dispatch(void)
{
    if (rln_decscusr_ok)
        return;
    rln_decscusr_ok = true;
    if (rln_phase == rln_phase_edit)
        rln_emit_mode_cursor();
}

// Feed one byte through the handshake mini-parser. Recognized shapes
// during prompt_cpr / width_cpr:
//   CPR:  \33 [ <digits> ; <digits> R
//   DA2:  \33 [ > <digits> ( ; <digits> )* c   (Secondary Device Attributes)
// Bytes that don't match a CPR candidate go to the typeahead ring so
// they can replay through the normal parser when phase = edit. DA2
// candidates do NOT use the release-on-mismatch buffer once entered
// (state 7) — xterm-class DA replies run 35+ bytes long and would
// otherwise overflow the 12-byte seq buffer, lose the final `c`, and
// time the handshake out. DA replies never originate from user input,
// so it's safe to consume them silently.
static void rln_cpr_feed(uint8_t b)
{
    // DA2-recognition state: process inline, no buffering, no length
    // cap. The `>` private marker is unique to server replies, so it
    // cannot conflict with typed input.
    if (rln_cpr_state == 7)
    {
        if (isdigit(b) || b == ';')
            ; // params discarded — presence of reply is the signal
        else if (b == 'c')
        {
            rln_cpr_state = 0;
            rln_da2_dispatch();
        }
        else
            rln_cpr_state = 0;
        return;
    }

    // States 0-3 / 5: CPR candidate (or pre-classification). Buffer
    // bytes so a mismatch can release them into typeahead — necessary
    // because typed input like `\33[A` (arrow up) shares the prefix.
    if (rln_cpr_seq_len >= RLN_CPR_SEQ_MAX)
    {
        rln_cpr_release_seq();
        rln_typeahead_push(b);
        return;
    }
    rln_cpr_seq[rln_cpr_seq_len++] = b;

    switch (rln_cpr_state)
    {
    case 0: // idle
        if (b == '\33')
            rln_cpr_state = 1;
        else
        {
            rln_cpr_seq_len = 0;
            rln_typeahead_push(b);
        }
        break;
    case 1: // saw ESC
        if (b == '[')
        {
            rln_cpr_state = 2;
            rln_cpr_p1 = 0;
        }
        else
            rln_cpr_release_seq();
        break;
    case 2: // saw CSI; classify CPR vs DA2
        if (b == '>')
        {
            // Discard the buffered `\33[>` — committed to DA2.
            rln_cpr_seq_len = 0;
            rln_cpr_state = 7;
        }
        else if (isdigit(b))
        {
            rln_cpr_state = 3;
            rln_cpr_p1 = b - '0';
        }
        else
            rln_cpr_release_seq();
        break;
    case 3: // CPR p1
        if (isdigit(b))
            rln_cpr_p1 = rln_cpr_p1 * 10 + (b - '0');
        else if (b == ';')
        {
            rln_cpr_state = 5;
            rln_cpr_p2 = 0;
        }
        else
            rln_cpr_release_seq();
        break;
    case 5: // CPR p2
        if (isdigit(b))
            rln_cpr_p2 = rln_cpr_p2 * 10 + (b - '0');
        else if (b == 'R')
        {
            rln_cpr_seq_len = 0;
            rln_cpr_state = 0;
            rln_cpr_dispatch(rln_cpr_p1, rln_cpr_p2);
        }
        else
            rln_cpr_release_seq();
        break;
    }
}

/* ----- Top-level task / lifecycle ----- */

void rln_read_line(rln_read_callback_t callback)
{
    rln_timeout_ms = 0;
    rln_buflen = 0;
    rln_bufpos = 0;
    rln_typed_ansi.state = ansi_state_C0;
    rln_typed_ansi.csi_param_count = 0;
    rln_typed_ansi.csi_param[0] = 0;
    rln_callback = callback;
    rln_history_pos = 0;
    rln_buf[0] = 0;
    rln_lastkey_len = 0;
    rln_keyseq_accum_len = 0;
    rln_keyseq_overflow = false;

    rln_phase = rln_phase_prompt_cpr;
    // Sentinels for the content-based CPR heuristic: 0 means
    // "not pinned yet". Must be cleared every call or replies
    // from the next handshake get misclassified.
    rln_prompt_col = 0;
    rln_term_height = 0;
    rln_term_width = 0;
    rln_cur_idx = 0;
    rln_rendered_max_row = 0;
    rln_decscusr_ok = false;
    rln_cpr_state = 0;
    rln_cpr_seq_len = 0;
    rln_typeahead_len = 0;
    rln_handshake_deadline = make_timeout_time_ms(RLN_HANDSHAKE_MS);

    // Fire the whole handshake in one burst so the cursor doesn't
    // visibly park at the bottom-right corner. Sequence:
    //   ?25l    hide cursor
    //   s       DECSC saves prompt position
    //   6n      CPR1 (prompt column)
    //   >c      DA2 — DECSCUSR-support probe; minicom mis-parses
    //           this and leaks the `c` as literal text at the
    //           prompt column...
    //   u       DECRC snaps cursor back to prompt column...
    //   K       ...and EL erases the leaked `c` (no-op on conforming
    //           peers; the line past the prompt char is empty)
    //   999;999H go to bottom-right
    //   6n      CPR2 (terminal height + width) — also the fence:
    //           burst order guarantees DA2's reply lands before this,
    //           so CPR2 marks "all knowable, transition to edit."
    //   u       DECRC back to prompt
    //   ?25h    show cursor
    // RCP runs from the burst rather than the CPR-reply path so the
    // cursor returns to the prompt even when the terminal doesn't
    // respond to CPR (pipes, dumb relays); the deadline fallback
    // inherits the right cursor placement for free. We do not touch
    // DECAWM; the user's terminal stays in its native wrap mode and
    // we trust the pending-wrap (xenl) model on margin writes.
    printf("\33[?25l\33[s\33[6n\33[>c\33[u\33[K\33[999;999H\33[6n\33[u\33[?25h");
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
    while (rln_callback)
    {
        int ch = stdio_getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT)
            break;
        rln_timer = make_timeout_time_ms(rln_timeout_ms);
        // Sniffer always runs; CPR/DA replies are absorbed even
        // mid-edit. Non-matching bytes are released by the parser to
        // either typeahead (handshake) or rln_line_rx_typed (edit)
        // via rln_typeahead_push's phase dispatch.
        rln_cpr_feed((uint8_t)ch);
        // Provide instant relief during handshake for scripting
        // tools which do not respond to ANSI sequences.
        if (rln_phase != rln_phase_edit &&
            (ch == '\r' || rln_typeahead_len >= RLN_TYPEAHEAD_MAX))
        {
            rln_term_width = 0; // infinite line mode
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
    rln_caps = 0;
    rln_phase = rln_phase_edit;
    rln_term_width = 0;
    rln_overwrite = false;
}

void rln_run(void)
{
    rln_enable_history = false;
    rln_max_length = 254; // reserve 1 for the stdin newline
    if (rln_decscusr_ok)
        printf("\33[0 q");
}

void rln_stop(void)
{
    // Don't spam resets during RIA memory transfers
    if (!ria_active())
        printf(STR_TERM_SOFT_RESET);
    // NFC launch also calls this
    if (rln_callback)
        rln_sync_cursor_to(rln_buflen);
    rln_init();
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

uint16_t rln_get_term_width(void)
{
    uint16_t cap = 80;
    if (vga_connected())
    {
        vga_canvas_t c = vga_get_canvas();
        if (c == vga_canvas_320_240 || c == vga_canvas_320_180)
            cap = 40;
    }
    if (rln_term_width == 0)
        return cap;
    return rln_term_width > cap ? cap : rln_term_width;
}

uint16_t rln_get_term_height(void)
{
    uint16_t cap = 0;
    if (vga_connected())
        cap = (vga_get_display_type() == 2) ? 32 : 30;
    if (rln_term_height > 0)
        return (cap && rln_term_height > cap) ? cap : rln_term_height;
    return cap ? cap : 24;
}

/* Readline magic: lastkey + peekpoke
 */

// int ria_readline_lastkey(char *key);
bool rln_api_lastkey(void)
{
    uint8_t len = 0;
    if (rln_callback && rln_lastkey_len)
    {
        len = rln_lastkey_len;
        // Push in reverse so the 6502 pops bytes in the order received.
        for (int i = len - 1; i >= 0; i--)
            if (!api_push_uint8(&rln_lastkey_buf[i]))
                return api_return_errno(API_EINVAL);
        rln_lastkey_len = 0;
    }
    return api_return_ax(len);
}

// int ria_readline_peek(char *peek);
bool rln_api_peek(void)
{
    if (!rln_callback)
        return api_return_ax(0);
    rln_buf[rln_buflen] = 0;
    char zero = 0;
    if (!api_push_char(&zero))
        return api_return_errno(API_EINVAL);
    for (int i = rln_buflen - 1; i >= 0; i--)
        if (!api_push_char(&rln_buf[i]))
            return api_return_errno(API_EINVAL);
    return api_return_ax(rln_bufpos);
}

// int ria_readline_poke(const char *poke);
bool rln_api_poke(void)
{
    const char *poke = (char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    if (!rln_callback)
        return api_return_ax(0);
    rln_ansi_t a = {.state = ansi_state_C0};
    for (const char *p = poke; *p; p++)
    {
        char ch = *p;
        rln_line_rx(&a, (uint8_t)ch);
        if (ch == '\r')
            break;
    }
    return api_return_ax(rln_bufpos);
}
