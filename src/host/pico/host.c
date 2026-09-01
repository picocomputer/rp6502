/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What both Pico firmwares say about themselves. What only one of them
 * says is in host_ria.c.
 *
 * TIMER0's raw 64-bit microsecond counter, free-running since reset.
 */

#include "host/host.h"

#include <pico/time.h> /* time_us_64 */

uint64_t host_clock_us(void)
{
    return time_us_64();
}
