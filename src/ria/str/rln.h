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

#endif /* _RIA_STR_RLN_H_ */
