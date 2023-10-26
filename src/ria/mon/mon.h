/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MON_H_
#define _MON_H_

#include <stdbool.h>
#include <stdint.h>

/* Kernel events
 */

void mon_task(void);
void mon_reset(void);

// Test if commands exists. Used to determine
// acceptable names when installing ROMs.
bool mon_command_exists(const char *buf, uint8_t buflen);

#endif /* _MON_H_ */
