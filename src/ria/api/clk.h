/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_CLK_H_
#define _RIA_API_CLK_H_

/* The CLK driver manages real-time counters.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void clk_init(void);
void clk_run(void);

// Print for status command.
void clk_print_status(void);

// Use POSIX TZ format. e.g. PST8PDT,M3.2.0/2,M11.1.0/2
const char *clk_set_time_zone(const char *tz);

/* The API implementation for time support
 */

bool clk_api_clock(void);
bool clk_api_get_res(void);
bool clk_api_get_time(void);
bool clk_api_set_time(void);
bool clk_api_get_time_zone(void);

#endif /* _RIA_API_CLK_H_ */
