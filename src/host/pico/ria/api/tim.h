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
bool tim_check_time_zone(const char *in, char *out);
void tim_apply_time_zone(const char *tz, bool changed);
int tim_time_zone_response(char *buf, size_t buf_size, int state, unsigned width);

/* The zone rides this machine's TIM row. core/api/tim.h has the clock and no
 * setting; this header re-rows it, and the pico roster includes this one. */
#include "core/api/tim.h"
#undef TIM_DRIVER
#define TIM_CONFIG_TZ CONFIG_STR(T, tim, time_zone, TIM_TZ_MAX_SIZE, "UTC0", \
    tim_check_time_zone, tim_apply_time_zone, STR_TZ, tim_time_zone_response, \
    STR_HELP_SET_TZ, tim_tzdata_response)
#define TIM_DRIVER DRIVER(tim_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    TIM_CONFIG_TZ, nul_config)

#endif /* _RIA_API_TIM_H_ */
