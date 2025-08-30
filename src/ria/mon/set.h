/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MON_SET_H_
#define _RIA_MON_SET_H_

/* Monitor commands for setting system config
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Monitor commands
 */

void set_mon_set(const char *args, size_t len);

#endif /* _RIA_MON_SET_H_ */
