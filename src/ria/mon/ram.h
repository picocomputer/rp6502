/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RAM_H_
#define _RAM_H_

#include <stddef.h>

/* Kernel events
 */

void ram_task(void);
bool ram_active(void);
void ram_reset(void);

/* Monitor commands
 */

void ram_mon_binary(const char *args, size_t len);
void ram_mon_address(const char *args, size_t len);

#endif /* _RAM_H_ */
