/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MON_MON_H_
#define _RIA_MON_MON_H_

/* Monitor command line and dispatch
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "sys/out.h"

/* Main events
 */

void mon_task(void);
void mon_stop(void);
void mon_break(void);

// The monitor's response queue is rendered through sys/out.c (see out.h for
// the out_source_fn contract).
typedef out_source_fn mon_response_fn;
void mon_add_response_fn(mon_response_fn fn); // state 0
void mon_add_response_fn_state(mon_response_fn fn, int state);
void mon_add_response_utf8(const char *utf8);
void mon_add_response_lfs(int result);
void mon_add_response_fatfs(int fresult);

// After queuing a preview, request a YES/no confirmation. cb() runs only if the
// user types YES; Ctrl-C, break, or anything else cancels back to the prompt.
typedef void (*mon_confirm_fn)(void);
void mon_response_confirm(mon_confirm_fn cb);

// Test if commands exists. Used to determine
// acceptable names when installing ROMs.
bool mon_command_exists(const char *buf);

#endif /* _RIA_MON_MON_H_ */
