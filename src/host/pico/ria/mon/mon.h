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

#include "core/api/api.h" /* api_errno: the seam's error currency */

/* Main events
 */

void mon_task(void);
void mon_init(void);
void mon_stop(void);
void mon_break(void);

// A response generator. The renderer calls it with the slot's state and the
// active wrap width; it snprintf()s the next chunk and returns the next state,
// or a negative state when there is no more. It is only guaranteed 80 columns
// plus a newline and null but may use the entire buffer. A call with a negative
// state means the response is being cancelled, so close any open files.
typedef int (*mon_response_fn)(char *buf, size_t size, int state, unsigned width);
void mon_add_response_fn(mon_response_fn fn); // state 0
void mon_add_response_fn_state(mon_response_fn fn, int state);
void mon_add_response_utf8(const char *utf8);
void mon_add_response_lfs(int result);
void mon_add_response_fatfs(int fresult);
void mon_add_response_errno(api_errno err); /* the seam's answers */

// After queuing a preview, request a YES/no confirmation. cb() runs only if the
// user types YES; Ctrl-C, break, or anything else cancels back to the prompt.
typedef void (*mon_confirm_fn)(void);
void mon_response_confirm(mon_confirm_fn cb);

// Test if commands exists. Used to determine
// acceptable names when installing ROMs.
bool mon_command_exists(const char *buf);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define MON_DRIVER DRIVER(mon_init, nul_task, mon_task, nul_run, mon_stop, mon_break, nul_config, nul_config)

#endif /* _RIA_MON_MON_H_ */
