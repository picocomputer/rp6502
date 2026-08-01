/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine's microsecond counter, read hi-lo-hi so a carry between the
 * two words never shows a torn value.
 */

#include "mmio.h"

#include <pico/time.h>

uint64_t time_us_64(void)
{
    uint32_t hi, lo;
    do
    {
        hi = MTIME_HI;
        lo = MTIME_LO;
    } while (hi != MTIME_HI);
    return ((uint64_t)hi << 32) | lo;
}
