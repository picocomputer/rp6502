/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _XIN_H_
#define _XIN_H_

#include <stdint.h>
#include <stdbool.h>

void xin_task(void);
int xin_pad_count(void);
bool xin_is_xbox_one(int slot);
bool xin_is_xbox_360(int slot);

#endif /* _XIN_H_ */
