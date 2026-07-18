/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for the pico-sdk pico/time.h. The vendored firmware reads "now"
 * through time_us_64; the emulator serves it from the deterministic master clock
 * the runner advances (main_clock_8, in emu/main.c), so run time is a reproducible
 * function of the frame count rather than host wall time. Inlined here — a shim is
 * the one place a static-inline definition is warranted (declare-in-.h/define-in-.c
 * everywhere else, for LTO).
 */

#ifndef _EMU_SHIM_PICO_TIME_H_
#define _EMU_SHIM_PICO_TIME_H_

#include <stdint.h>
#include <stdbool.h>

uint64_t main_clock_8(void); /* the emulator's master clock, in 1/8-ticks (emu/main.c) */

static inline uint64_t time_us_64(void)
{
    return main_clock_8() / 2048; /* 2048 eighth-ticks/us (256 MHz -> 256 ticks/us) */
}

typedef uint64_t absolute_time_t;

static inline absolute_time_t make_timeout_time_us(int64_t us)
{
    return time_us_64() + (us < 0 ? 0 : (uint64_t)us);
}

static inline absolute_time_t make_timeout_time_ms(int64_t ms)
{
    return time_us_64() + (ms < 0 ? 0 : (uint64_t)ms * 1000);
}

static inline bool time_reached(absolute_time_t t)
{
    return time_us_64() >= t;
}

#endif /* _EMU_SHIM_PICO_TIME_H_ */
