/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What BOTH Pico firmwares answer of core/host.h -- the parts of the contract
 * whose callers are split across them: core/hid/kbd.c is the RIA's,
 * core/term/term.c is the VGA's. What only one of them answers is in
 * host_ria.c. Both build with IPO, so this inlines back to the SDK call.
 *
 * The clock is TIMER0's raw 64-bit microsecond counter, free-running since
 * reset.
 */

#include "host.h"

uint64_t host_clock_us(void)
{
    return time_us_64();
}
