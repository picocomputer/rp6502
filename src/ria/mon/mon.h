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

/* Main events
 */

void mon_task(void);
void mon_stop(void);
void mon_break(void);

// Monitor response system paginates without blocking.
// A mon_response_fn will snprintf to the buffer and
// return the state with which to be called next with.
// A mon_response_fn is only guaranteed 80 columns plus
// a newline and null but may use the entire buffer.
// A mon_response_fn will return a negative state when
// there is no more work.
// If mon_response_fn is called with a negative state,
// the response is being cancelled so close any open
// files.
typedef int (*mon_response_fn)(char *, size_t, int);
void mon_add_response_fn(mon_response_fn fn); // state 0
void mon_add_response_fn_state(mon_response_fn fn, int state);
void mon_add_response_utf8(const char *utf8);
void mon_add_response_lfs(int result);
void mon_add_response_fatfs(int fresult);

// Print a FatFs error immediately (for run-phase tasks that own the terminal and
// print progress directly, rather than queuing via mon_add_response_fatfs).
void mon_print_fatfs(int fresult);

// After queuing a preview, request a YES/no confirmation. cb() runs only if the
// user types YES; Ctrl-C, break, or anything else cancels back to the prompt.
typedef void (*mon_confirm_fn)(void);
void mon_response_confirm(mon_confirm_fn cb);
// Run cb once the response queue drains, with no prompt (read-only continuation).
void mon_response_then(mon_confirm_fn cb);

// Test if commands exists. Used to determine
// acceptable names when installing ROMs.
bool mon_command_exists(const char *buf);

#endif /* _RIA_MON_MON_H_ */
