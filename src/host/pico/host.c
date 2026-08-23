/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What a Pico answers of core/host.h. Only the clock so far: TIMER0's raw
 * 64-bit microsecond counter, free-running since reset.
 *
 * Its own translation unit because the two firmwares split the callers --
 * core/hid/kbd.c is the RIA's, core/term/term.c is the VGA's -- and this
 * directory is the one place both builds already share. Both build with IPO,
 * so it inlines back to the SDK call.
 */

#include "host.h"

uint64_t host_clock_us(void)
{
    return time_us_64();
}
