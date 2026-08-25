/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_TIM_H_
#define _RIA_API_TIM_H_

/* The time zone, as only a machine with a monitor and a settings store has
 * one: a database to page through, a status column, and a POSIX string that
 * survives a power cycle. The clock itself is core/api/tim.h. */

#include <stddef.h>
#include <stdbool.h>

#define TIM_TZ_MAX_SIZE 64

// Print for status command
int tim_status_response(char *buf, size_t buf_size, int state, unsigned width);

// Show tz database
int tim_tzdata_response(char *buf, size_t buf_size, int state, unsigned width);

// Configuration setting TZ
// Use POSIX TZ format. e.g. PST8PDT,M3.2.0/2,M11.1.0/2
void tim_load_time_zone(const char *str);
bool tim_set_time_zone(const char *tz);
const char *tim_get_time_zone(void);

#endif /* _RIA_API_TIM_H_ */
