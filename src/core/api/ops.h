/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_OPS_H_
#define _CORE_API_OPS_H_

#include <stdbool.h>
#include <stdint.h>

/* Run one op. An op with no handler is ENOSYS. */
bool ops_dispatch(uint8_t operation);

#endif /* _CORE_API_OPS_H_ */
