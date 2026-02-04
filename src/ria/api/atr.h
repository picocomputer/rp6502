/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_ATR_H_
#define _RIA_API_ATR_H_

/* The ATR driver dispatches get/set attribute calls to actual data sources.
 * State for readline configuration lives in std.c.
 * State for system attributes lives in their respective modules.
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

/* Deprecated API
 */

bool atr_api_phi2(void);
bool atr_api_code_page(void);
bool atr_api_lrand(void);
bool atr_api_stdin_opt(void);
bool atr_api_errno_opt(void);

#endif /* _RIA_API_ATR_H_ */
