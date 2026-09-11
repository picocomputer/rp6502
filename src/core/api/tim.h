/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_TIM_H_
#define _CORE_API_TIM_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

void tim_init(void);
void tim_stop(void);

bool tim_get_time(struct timespec *ts);
bool tim_set_time(const struct timespec *ts);
void tim_get_time_res(struct timespec *ts);

// Broken-down time, local zone or UTC. False when t is out of range.
bool tim_localtime(time_t t, struct tm *out);
bool tim_gmtime(time_t t, struct tm *out);

// strftime, with the result converted to the active OEM code page.
size_t tim_strftime(char *dst, size_t max, const char *format, const struct tm *tm);

#define TIM_DRIVER DRIVER(tim_init, nul_task, nul_task, nul_run, tim_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _CORE_API_TIM_H_ */
