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

// This handles pagination without blocking.
typedef int (*mon_response_fn)(char *, size_t, int);
void mon_add_response_fn(int (*fn)(char *, size_t, int));
void mon_add_response_fn_state(mon_response_fn fn, int state);
void mon_add_response_str(const char *str);
void mon_add_response_lfs(int result);
void mon_add_response_fatfs(int fresult);

// Test if commands exists. Used to determine
// acceptable names when installing ROMs.
bool mon_command_exists(const char *buf, size_t buflen);

#endif /* _RIA_MON_MON_H_ */
