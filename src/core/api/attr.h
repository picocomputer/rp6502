/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_ATTR_H_
#define _CORE_API_ATTR_H_

/* The ATR driver dispatches get/set attribute calls.
 * The API allows for 256 attributes of 31 bits.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* The API implementation
 */

bool attr_api_get(void);
bool attr_api_set(void);

/* Deprecated API
 */

bool attr_api_phi2(void);
bool attr_api_code_page(void);
bool attr_api_lrand(void);
bool attr_api_errno_opt(void);

#endif /* _CORE_API_ATTR_H_ */
