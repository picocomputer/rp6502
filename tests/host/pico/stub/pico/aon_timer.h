/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TEST_PICO_AON_TIMER_H_
#define _TEST_PICO_AON_TIMER_H_

#include <stdbool.h>
#include <time.h>

bool aon_timer_get_time(struct timespec *ts);

#endif /* _TEST_PICO_AON_TIMER_H_ */
