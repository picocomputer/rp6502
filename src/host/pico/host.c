/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What both Pico firmwares answer of host/os.h. What only one of them
 * answers is in host_ria.c.
 *
 * TIMER0's raw 64-bit microsecond counter, free-running since reset.
 */

#include "host.h"

uint64_t host_clock_us(void)
{
    return time_us_64();
}
