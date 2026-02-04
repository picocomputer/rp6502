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

void rln_task(void);
void rln_run(void);
void rln_stop(void);
void rln_break(void);

// Timeout is true when input is idle too long.
// Requesting a timeout of 0 ms will disable the idle timer.
typedef void (*rln_read_callback_t)(bool timeout, const char *buf, size_t length);

// Prepare the line editor. The rln module can read entire lines
// of input with basic editing on ANSI terminals.
void rln_read_line(uint32_t timeout_ms, rln_read_callback_t callback, uint8_t maxlen, uint32_t ctrl_bits);

#endif /* _RIA_STR_RLN_H_ */
