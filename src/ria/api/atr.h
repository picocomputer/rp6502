/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_ATR_H_
#define _RIA_API_ATR_H_

/* The ATR driver dispatches get/set attribute calls.
 * The API allows for 256 attributes of 31 bits.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* The API implementation
 */

bool atr_api_get(void);
bool atr_api_set(void);

/* Deprecated API
 */

bool atr_api_phi2(void);
bool atr_api_code_page(void);
bool atr_api_lrand(void);
bool atr_api_errno_opt(void);

#endif /* _RIA_API_ATR_H_ */
