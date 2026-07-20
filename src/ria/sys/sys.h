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
void sys_run(void);

/* Monitor commands
 */

void sys_mon_reboot(const char *args);
void sys_mon_reset(const char *args);
void sys_mon_status(const char *args);

// 6502 run time in ticks of us_per_tick microseconds
uint32_t sys_get_run(uint32_t us_per_tick);

#endif /* _RIA_SYS_SYS_H_ */
