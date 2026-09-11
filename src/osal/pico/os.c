/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "osal/os.h"

#include <pico/rand.h>
#include <pico/time.h>

/* TIMER0's microsecond counter, which is the same one host_clock_us reads,
 * because on a board machine time is real time and neither can drift from the
 * other. */
uint64_t os_mono_ns(void)
{
    return time_us_64() * 1000;
}

uint32_t os_random(void)
{
    uint64_t s = get_rand_64();
    return (uint32_t)(s ^ (s >> 32));
}
