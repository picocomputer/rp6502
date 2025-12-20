/*
 * Copyright (c) 2025 Rumbledethumps
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
void mon_add_response_str(const char *str);
void mon_add_response_lfs(int result);
void mon_add_response_fatfs(int fresult);

// Test if commands exists. Used to determine
// acceptable names when installing ROMs.
bool mon_command_exists(const char *buf, size_t buflen);

#endif /* _RIA_MON_MON_H_ */
