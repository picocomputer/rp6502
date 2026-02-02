/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_ATR_H_
#define _RIA_API_ATR_H_

/* The ATR driver dispatches get/set attribute
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void atr_run(void);

/* The API implementation
 */

bool atr_api_get(void);
bool atr_api_set(void);
bool atr_api_set_readline(void);

#endif /* _RIA_API_ATR_H_ */
