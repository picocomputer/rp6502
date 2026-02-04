/*
 * Copyright (c) 2025 Rumbledethumps
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
typedef void (*rln_read_callback_t)(bool timeout, const char *buf, size_t length);

// Prepare the line editor. The rln module can read entire
// lines of input with editing on ANSI terminals.
void rln_read_line(rln_read_callback_t callback);

// Read line without history and with a temporary timeout override.
// The timeout_ms parameter temporarily overrides the configured timeout.
void rln_read_line_programmatic(rln_read_callback_t callback, uint32_t timeout_ms);

/* Readline configuration getters/setters */

bool rln_get_suppress_end_move(void);
void rln_set_suppress_end_move(bool v);

bool rln_get_suppress_newline(void);
void rln_set_suppress_newline(bool v);

bool rln_get_enable_history(void);
void rln_set_enable_history(bool v);

uint8_t rln_get_max_length(void);
void rln_set_max_length(uint8_t v);

uint32_t rln_get_timeout(void);
void rln_set_timeout(uint32_t v);

uint32_t rln_get_ctrl_bits(void);
void rln_set_ctrl_bits(uint32_t v);

uint8_t rln_get_cursor_pos(void);
void rln_set_cursor_pos(uint8_t v);

uint8_t rln_get_end_char(void);

bool rln_get_timed_out(void);

#endif /* _RIA_STR_RLN_H_ */
