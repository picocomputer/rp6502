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

// Test if commands exists. Used to determine
// acceptable names when installing ROMs.
bool mon_command_exists(const char *buf, size_t buflen);

#endif /* _RIA_MON_MON_H_ */
