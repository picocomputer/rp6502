/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_PRO_H_
#define _RIA_API_PRO_H_

/* The process manager handles argv and launching other ROMs.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* The API implementation
 */

bool pro_api_argv(void);
bool pro_api_execv(void);

#endif /* _RIA_API_PRO_H_ */
