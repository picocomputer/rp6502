/*
 * Copyright (c) 2023 Rumbledethumps
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

/* Kernel events
 */

void ram_task(void);
bool ram_active(void);
void ram_reset(void);

/* Monitor commands
 */

void ram_mon_binary(const char *args, size_t len);
void ram_mon_address(const char *args, size_t len);

#endif /* _RIA_MON_RAM_H_ */
