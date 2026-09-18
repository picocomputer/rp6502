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

#include "core/api/api.h"

/* Main events
 */

void mon_task(void);
void mon_init(void);
void mon_stop(void);
void mon_break(void);

// mon_task calls a queued response generator with its current state and the
// terminal width. The generator writes the next chunk into buf and returns the
// next state, or a negative state when there is no more. The buffer is only
// guaranteed to hold 80 columns plus a newline and a null, but a generator may
// use the entire buffer. A call with a negative state means the response is
// being cancelled, so the generator must close any files it has open.
typedef int (*mon_response_fn)(char *buf, size_t size, int state, unsigned width);
void mon_add_response_fn(mon_response_fn fn); // state 0
void mon_add_response_fn_state(mon_response_fn fn, int state);
void mon_add_response_utf8(const char *utf8);
void mon_add_response_lfs(int result);
void mon_add_response_fatfs(int fresult);
void mon_add_response_errno(api_errno err);

typedef void (*mon_confirm_fn)(void);
void mon_response_confirm(mon_confirm_fn cb);

// Test if commands exists. Used to determine
// acceptable names when installing ROMs.
bool mon_command_exists(const char *buf);

#define MON_DRIVER DRIVER(mon_init, nul_task, mon_task, nul_run, mon_stop, mon_break, nul_config, nul_config, nul_sst)

#endif /* _RIA_MON_MON_H_ */
