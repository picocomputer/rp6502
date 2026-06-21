/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sys/out.h"
#include <string.h>

// Minimum column budget remaining after the indent for wrap-with-indent to
// engage. If the BEL marker lands too close to the right edge the indent is
// suppressed and wrapped lines fall back to column 0.
#define OUT_INDENT_MIN_WRAP 20

// Drain w->cur to the sink with word-wrap. Returns true when the sink paused
// (more to emit later), false when cur is fully drained. A sink that returns
// false has not accepted the byte; the unadvanced cursor re-offers it.
static bool out_flush(out_wrap_t *w, out_sink sink, bool more)
{
    char c;
    while ((c = w->cur[w->pos]) != 0)
    {
        // BEL marks the indent column for subsequent wraps; consume it
        // silently without emitting, so it can never pause.
        if (w->indent_pending == 0 && c == '\a')
        {
            w->indent = ((int)w->width - w->col >= OUT_INDENT_MIN_WRAP) ? w->col : 0;
            w->pos++;
            continue;
        }
        // Emit one queued indent space per iteration after a wrap, so wrapped
        // continuation lines resume at the column marked by the producer's BEL.
        if (w->indent_pending > 0)
        {
            if (!sink(' '))
                return true;
            w->indent_pending--;
            w->col++;
            continue;
        }
        // Word wrap on space: peek the next word's length, spanning into the
        // staged buffer when the word crosses the fill boundary.
        if (!w->width_aware && c == ' ')
        {
            int n = w->pos + 1;
            while (w->cur[n] && w->cur[n] != ' ' && w->cur[n] != '\n' && w->cur[n] != '\r')
                n++;
            int next_word_len = n - w->pos - 1;
            bool word_complete = w->cur[n] != 0;
            if (!word_complete && w->next_loaded)
            {
                int m = 0;
                while (w->next[m] && w->next[m] != ' ' && w->next[m] != '\n' && w->next[m] != '\r')
                    m++;
                next_word_len += m;
                word_complete = w->next[m] != 0 || !more;
            }
            else if (!word_complete)
            {
                word_complete = !more;
            }
            if (!word_complete || w->col + 1 + next_word_len > (int)w->width)
            {
                if (!sink('\n'))
                    return true;
                w->pos++; // drop the space
                w->col = 0;
                w->indent_pending = w->indent;
                continue;
            }
        }
        // Hard newline for a glyph that would overflow the line — catches words
        // longer than the line that the word-wrap branch could not break.
        if (!w->width_aware && (unsigned char)c >= 0x20 && w->col >= (int)w->width)
        {
            if (!sink('\n'))
                return true;
            w->col = 0;
            w->indent_pending = w->indent;
            continue; // re-loop without advancing pos
        }
        if (!sink(c))
            return true;
        w->pos++;
        if (c == '\n')
        {
            w->col = 0;
            w->indent = 0;
        }
        else if (c == '\r')
            w->col = 0;
        else if (c == '\b')
        {
            if (w->col > 0)
                w->col--;
        }
        else if ((unsigned char)c < 0x20)
            // A control byte whose on-screen width we can't track; stop
            // wrap/column injection for the rest of the chain.
            w->width_aware = true;
        else
            w->col++;
    }
    w->pos = -1;
    return false;
}

bool out_render(out_wrap_t *w, out_source_fn fn, int *state, out_sink sink, bool step)
{
    if (!w->cur)
    {
        w->cur = w->buf_a;
        w->next = w->buf_b;
        w->pos = -1;
    }
    for (;;)
    {
        // Promote a staged next to cur once cur is drained — no copy.
        if (w->pos < 0 && w->next_loaded)
        {
            char *tmp = w->cur;
            w->cur = w->next;
            w->next = tmp;
            w->pos = 0;
            w->next_loaded = false;
        }
        // Stage the next chunk so the cur about to be flushed has lookahead.
        if (!w->next_loaded && *state >= 0)
        {
            w->next[0] = 0;
            *state = fn(w->next, OUT_BUF_SIZE, *state, w->width);
            w->next_loaded = (w->next[0] != 0);
            // An empty fill with nothing buffered is an async await (e.g. a
            // scan still running) — retry later.
            if (!w->next_loaded && w->pos < 0)
                return *state >= 0;
            // Step mode: yield after the generator call so the caller's main
            // loop runs (the generator may have started a RIA read) before the
            // chunk is flushed.
            if (step)
                return true;
            continue;
        }
        // Flush cur (next holds the cross-fill lookahead).
        if (w->pos >= 0)
        {
            if (out_flush(w, sink, *state >= 0))
                return true; // sink paused
            continue; // cur drained; loop to swap/stage
        }
        return false; // generator done and nothing buffered
    }
}

bool out_pending(const out_wrap_t *w)
{
    return w->cur && (w->pos >= 0 || w->next_loaded);
}

void out_cancel(out_wrap_t *w, out_source_fn fn, int *state)
{
    if (fn && *state >= 0)
        fn(w->cur ? w->cur : w->buf_a, OUT_BUF_SIZE, -1, w->width);
    *state = -1;
    w->pos = -1;
    w->next_loaded = false;
    w->col = 0;
    w->indent = 0;
    w->indent_pending = 0;
    w->width_aware = false;
    if (!w->cur)
    {
        w->cur = w->buf_a;
        w->next = w->buf_b;
    }
}
