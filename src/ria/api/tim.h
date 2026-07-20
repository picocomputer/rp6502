/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_TIM_H_
#define _RIA_API_TIM_H_

#define TIM_TZ_MAX_SIZE 64

/* The TIM driver owns the real time clock and the time zone.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* Main events
 */

void tim_init(void);

// Print for status command
int tim_status_response(char *buf, size_t buf_size, int state, unsigned width);

// Show tz database
int tim_tzdata_response(char *buf, size_t buf_size, int state, unsigned width);

// Configuration setting TZ
// Use POSIX TZ format. e.g. PST8PDT,M3.2.0/2,M11.1.0/2
void tim_load_time_zone(const char *str);
bool tim_set_time_zone(const char *tz);
const char *tim_get_time_zone(void);

// POSIX tzset() globals, which every libc spells differently
int tim_get_tz_daylight(void);
long tim_get_tz_offset(void);
const char *tim_get_tz_name(bool dst);

// Real time clock
bool tim_get_time(struct timespec *ts);
bool tim_set_time(const struct timespec *ts);
void tim_get_time_res(struct timespec *ts);

// Broken-down time, local zone or UTC. False when t is out of range.
bool tim_localtime(time_t t, struct tm *out);
bool tim_gmtime(time_t t, struct tm *out);

// strftime emitting code page text
size_t tim_strftime(char *dst, size_t max, const char *format, const struct tm *tm);

#endif /* _RIA_API_TIM_H_ */
