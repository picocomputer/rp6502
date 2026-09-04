/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_TIM_H_
#define _CORE_API_TIM_H_

/* The TIM driver owns the real time clock and the time zone. What a machine
 * with a monitor and a settings store does with the zone -- the tz database,
 * the status column, the POSIX string it loads -- is that machine's, and is
 * declared beside it. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* Main events
 */

void tim_init(void);

// Real time clock
bool tim_get_time(struct timespec *ts);
bool tim_set_time(const struct timespec *ts);
void tim_get_time_res(struct timespec *ts);

// Broken-down time, local zone or UTC. False when t is out of range.
bool tim_localtime(time_t t, struct tm *out);
bool tim_gmtime(time_t t, struct tm *out);

// strftime emitting code page text
size_t tim_strftime(char *dst, size_t max, const char *format, const struct tm *tm);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define TIM_DRIVER DRIVER(tim_init, nul_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _CORE_API_TIM_H_ */
