/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_REM_H_
#define _RIA_SYS_REM_H_

/* Remote console server.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void rem_init(void);
void rem_task(void);

/* Console tee hooks (called from com.c)
 */

void rem_tx(char ch);
int rem_rx(char *buf, int length);

/* Configuration
 */

void rem_load_port(const char *str);
void rem_load_key(const char *str);
bool rem_set_port(uint32_t port);
bool rem_set_key(const char *key);
uint16_t rem_get_port(void);
const char *rem_get_key(void);

#endif /* _RIA_SYS_REM_H_ */
