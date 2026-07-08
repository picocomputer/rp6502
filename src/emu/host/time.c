/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host backend for the pico-sdk monotonic clock (pico/time.h). The vendored
 * firmware reads "now" through time_us_64; the emulator serves it from the
 * deterministic master clock, so run time is a reproducible function of the
 * frame count rather than host wall time.
 */

#include "pico/time.h"
#include "emu/sys/cpu.h"

//TODO cpu_clock_8/master_8 is not a CPU thing. all that belong here.

uint64_t time_us_64(void)
{
    /* 256 MHz -> 256 ticks/us -> 2048 eighth-ticks/us. */
    return cpu_clock_8() / 2048;
}
