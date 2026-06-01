/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_STR_RLN_H_
#define _RIA_STR_RLN_H_

/* Readline-like line editor.
 */

#include <stddef.h>
#include <stdint.h>
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

// Terminal width. Priority: rln_set_term_width override if set, then
// the most recent CPR-reported width, then a VGA-aware fallback (40
// for 320-wide canvases, 80 otherwise) when VGA is connected, then 80.
uint16_t rln_get_term_width(void);

// Terminal height. Priority: rln_set_term_height override if set, then
// the most recent CPR-reported height, then a VGA-aware fallback (32
// in display mode 2, 30 otherwise) when VGA is connected, then 24.
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

#endif /* _RIA_STR_RLN_H_ */
