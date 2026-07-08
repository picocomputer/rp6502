/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator's master timebase and the pico-sdk monotonic clock (pico/time.h)
 * it backs. The vendored firmware reads "now" through time_us_64; the emulator
 * serves it from a deterministic master clock the 6502 advances, so run time is
 * a reproducible function of the frame count rather than host wall time.
 */

#include "emu/host/time.h"
#include "pico/time.h"

/* The master clock in 1/8-of-a-256MHz-tick units. Held that fine so the PHI2
 * fractional divider lands on an integer per-cycle step. Wraps in centuries. */
static uint64_t master_8;

void time_reset(void) { master_8 = 0; }
uint64_t time_clock_8(void) { return master_8; }
void time_advance_8(uint32_t ticks) { master_8 += ticks; }
void time_set_8(uint64_t ticks) { master_8 = ticks; }

uint64_t time_us_64(void)
{
    /* 256 MHz -> 256 ticks/us -> 2048 eighth-ticks/us. */
    return master_8 / 2048;
}

absolute_time_t make_timeout_time_us(int64_t us)
{
    return time_us_64() + (us < 0 ? 0 : (uint64_t)us);
}

absolute_time_t make_timeout_time_ms(int64_t ms)
{
    return time_us_64() + (ms < 0 ? 0 : (uint64_t)ms * 1000);
}

bool time_reached(absolute_time_t t)
{
    return time_us_64() >= t;
}
