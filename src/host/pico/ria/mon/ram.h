/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MON_RAM_H_
#define _RIA_MON_RAM_H_

/* Monitor commands to inspect or change 6502 RAM.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void ram_task(void);
void ram_break(void);

// True when more work is pending.
bool ram_active(void);

/* Monitor commands
 */

void ram_mon_binary(const char *args);
void ram_mon_address(const char *args);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define RAM_DRIVER DRIVER(nul_init, ram_task, nul_task, nul_run, nul_stop, ram_break, nul_config, nul_config)

#endif /* _RIA_MON_RAM_H_ */
