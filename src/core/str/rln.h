/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_STR_RLN_H_
#define _CORE_STR_RLN_H_

/* Readline-like line editor.
 */

#include <stddef.h>
#include <stdint.h>
#include "core/sys/sst.h"
#include <stdbool.h>

/* Main events
 */

void rln_init(void);
void rln_task(void);
void rln_run(void);
void rln_stop(void);
void rln_break(void);

// Timeout is true when input is idle too long.
// Requesting a timeout of 0 ms will disable the idle timer.
typedef void (*rln_read_callback_t)(bool timeout, const char *buf);

// Prepare the line editor. The rln module can read entire
// lines of input with editing on ANSI terminals.
void rln_read_line(rln_read_callback_t callback);

// Read line without history and with a temporary timeout override.
// The timeout_ms parameter temporarily overrides the configured timeout.
void rln_read_line_timeout(rln_read_callback_t callback, uint32_t timeout_ms);

// Read a line that is not recorded in history (e.g. a YES/no confirmation),
// keeping the normal (disabled) idle timeout.
void rln_read_line_no_history(rln_read_callback_t callback);

// Give up a read in progress: the callback never fires.
void rln_read_cancel(void);

/* Give up a read the way an input that ran out gives it up: what has been
 * typed so far is still a line, so the callback fires with it rather than
 * the read being thrown away. False when nothing was held, so a caller can
 * tell an unfinished line from an empty one. */
bool rln_read_flush(void);

// 6502 applications may configure the max length
void rln_set_max_length(uint8_t v);
uint8_t rln_get_max_length(void);

// Caps mode: 0=normal, 1=all caps, 2=invert
void rln_set_caps(uint8_t v);
uint8_t rln_get_caps(void);

// Suppress the line-terminating newline on completion (0=off, 1=on), so
// field input on the last line keeps the cursor on its row. Persists
// across reads; reset on stop().
void rln_set_suppress_nl(uint8_t v);
uint8_t rln_get_suppress_nl(void);

// Terminal geometry overrides. Setting non-zero pins the value and skips
// the CPR2 handshake when both axes are pinned. 0 = auto-detect.
void rln_set_term_width(uint16_t v);
void rln_set_term_height(uint16_t v);

// Telnet NAWS terminal size. Outranks CPR/VGA/default but yields to the
// rln_set_term_* override. (0,0) clears it; that and any mid-edit width
// change reflow the current input line at the new effective width.
void rln_set_naws_size(uint16_t w, uint16_t h);

// A source's far end has gone. What a source knows about its wire outlives a
// read and a program, which is what makes type-ahead and a CRLF pair work
// across both -- but not a session: the next client is a different terminal
// and owes nothing the last one did. Takes a com_source_t.
void rln_forget_source(unsigned src);

// Terminal width. Priority: rln_set_term_width override if set, then the
// telnet NAWS width, then the highest-priority terminal's CPR width (telnet
// over uart), then a VGA-aware fallback (40 for 320-wide canvases, 80
// otherwise) when VGA is connected, then 80.
uint16_t rln_get_term_width(void);

// Terminal height. Priority: rln_set_term_height override if set, then the
// telnet NAWS height, then the highest-priority terminal's CPR height (telnet
// over uart), then a VGA-aware fallback (32 in display mode 2, 30 otherwise)
// when VGA is connected, then 24.
uint16_t rln_get_term_height(void);

// Inject a sequence of input bytes into the active readline. CR ends
// the line normally; any other C0 control byte (0x00-0x1F) except ESC
// (which begins a CSI sequence) and CAN (0x18, which aborts one) also
// finishes the line, without adding to history. Controls other than CR
// echo as caret notation (^@..^_) when readline owns the room (0x03 as
// ^C), without being inserted. Poked bytes are dispatched in overwrite
// mode. A poke that arrives while a previous line's completion is still
// being deferred (the line is already submitted) is a no-op.
void rln_poke(const char *str);

// 6502 API entry points.
bool rln_api_lastkey(void);
bool rln_api_peek(void);
bool rln_api_poke(void);

/* The line the reader is building, which std.c's stdin bridge points into.
 * A savestate rebuilds that pointer from here rather than carrying it, and
 * bounds its own index into it against this length. */
#define RLN_LINE_MAX 256
const char *rln_line(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
/* The line being edited, the history behind it, the terminal this reader has
 * worked out it is talking to, and one parser per input source.
 *
 * The reader itself is a function pointer and rides as a token instead: on a
 * software machine there is exactly one, std.c's stdin bridge, and rebuilding
 * it by calling rln_read_line would emit the whole handshake and wipe the
 * line the blob just restored.
 *
 * The three deadlines are machine time, so they are carried whole rather than
 * re-armed: a deadline against the beam means the same thing in the machine
 * that loads the blob as in the one that made it. */
#define RLN_SST_SIZE 3629
void rln_sst_save(sst_cursor_t *c, unsigned flags);
bool rln_sst_load(sst_cursor_t *c, unsigned flags);

#define RLN_DRIVER DRIVER(rln_init, nul_task, rln_task, rln_run, rln_stop, rln_break, \
    nul_config, nul_config, SST(RLN_, 1, RLN_SST_SIZE, rln_sst_save, rln_sst_load))

#endif /* _CORE_STR_RLN_H_ */
