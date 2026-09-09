/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_ATTR_H_
#define _CORE_API_ATTR_H_

/* Each attribute carries 31 bits, because the 6502 call returns a signed long
 * and -1 is its error return.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool attr_api_get(void);
bool attr_api_set(void);

/* Deprecated single-purpose ops 0x02, 0x03, 0x04 and 0x06, which the two
 * calls above replace. Retained for binaries built with older SDKs.
 */

bool attr_api_phi2(void);
bool attr_api_code_page(void);
bool attr_api_lrand(void);
bool attr_api_errno_opt(void);

#endif /* _CORE_API_ATTR_H_ */
