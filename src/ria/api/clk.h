/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_CLK_H_
#define _RIA_API_CLK_H_

#define CLK_TZ_MAX_SIZE 64

/* The CLK driver manages real-time counters.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void clk_init(void);
void clk_run(void);

// Print for status command
int clk_status_response(char *buf, size_t buf_size, int state);

// Show tz database
int clk_tzdata_response(char *buf, size_t buf_size, int state);

// Configuration setting TZ
// Use POSIX TZ format. e.g. PST8PDT,M3.2.0/2,M11.1.0/2
void clk_load_time_zone(const char *str, size_t len);
bool clk_set_time_zone(const char *tz);
const char *clk_get_time_zone(void);

/* The API implementation for time support
 */

bool clk_api_tzset(void);
bool clk_api_tzquery(void);
bool clk_api_clock(void);
bool clk_api_get_res(void);
bool clk_api_get_time(void);
bool clk_api_set_time(void);

#endif /* _RIA_API_CLK_H_ */
