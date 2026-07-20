/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_SYS_H_
#define _RIA_SYS_SYS_H_

/* System monitor commands.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void sys_init(void);

/* Monitor commands
 */

void sys_mon_reboot(const char *args);
void sys_mon_reset(const char *args);
void sys_mon_status(const char *args);

#endif /* _RIA_SYS_SYS_H_ */
