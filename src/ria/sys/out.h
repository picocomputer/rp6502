/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_OUT_H_
#define _RIA_SYS_OUT_H_

/* Shared word-wrap formatter for response generators.
 *
 * Both the monitor and the modem drive their own response queues through this
 * one formatter. The caller owns the queue and the wrap cursor (out_wrap_t) and
 * supplies a sink; out_ owns only the word-wrap and a lookahead buffer.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define OUT_BUF_SIZE 128

// A response generator. out_render calls it with the slot's state and the
// active wrap width; it snprintf()s the next chunk and returns the next state,
// or a negative state when there is no more. It is only guaranteed 80 columns
// plus a newline and null but may use the entire buffer. A call with a negative
// state means the response is being cancelled, so close any open files.
typedef int (*out_source_fn)(char *buf, size_t size, int state, unsigned width);

// Receives one canonical output byte (printable or '\n'), applies the surface's
// line-ending policy, and returns false to apply backpressure (out_render then
// pauses; the caller resumes it on the next tick/read).
typedef bool (*out_sink)(char ch);

// Caller-owned wrap cursor; statically allocated, never malloc'd. The lookahead
// double-buffer lives here so each caller (the monitor, each modem descriptor)
// resumes independently.
typedef struct
{
    unsigned width; // wrap width; set before driving (monitor: term; modem: 80)
    char buf_a[OUT_BUF_SIZE];
    char buf_b[OUT_BUF_SIZE];
    char *cur;
    char *next;
    int pos; // -1 when cur is exhausted
    bool next_loaded;
    int col;
    int indent;
    int indent_pending;
    bool width_aware;
} out_wrap_t;

// Render one source through word-wrap to sink, wrapping to w->width and feeding
// fn(buf, size, *state, w->width) for chunks. Returns true while output remains
// (the sink paused, or — in step mode — after a generator call); false when fn
// returned a negative state and nothing is buffered (advance the queue). *state
// carries the generator's resumable state across calls.
//
// step: yield (return) after every generator call so the caller's main loop can
// run before the chunk is flushed. The monitor needs this because a generator
// may start a RIA read (going busy) whose output must not print until it
// finishes. The modem passes false to render straight through to its read
// buffer (its generators have no such side effects).
bool out_render(out_wrap_t *w, out_source_fn fn, int *state, out_sink sink, bool step);

// True while w holds rendered bytes not yet flushed to the sink. The caller must
// keep calling out_render while this is true even after its generator state has
// gone negative, or the trailing buffered output is lost.
bool out_pending(const out_wrap_t *w);

// Cancel a half-drained source: call fn with a negative state (cleanup) and
// reset the cursor.
void out_cancel(out_wrap_t *w, out_source_fn fn, int *state);

#endif /* _RIA_SYS_OUT_H_ */
