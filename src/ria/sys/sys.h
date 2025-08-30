/*
 * Copyright (c) 2025 Rumbledethumps
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

void sys_mon_reboot(const char *args, size_t len);
void sys_mon_reset(const char *args, size_t len);
void sys_mon_status(const char *args, size_t len);

#endif /* _RIA_SYS_SYS_H_ */
