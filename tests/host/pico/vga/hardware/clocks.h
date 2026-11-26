/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The system clock the RIA runs at (SYS_RP2350_KHZ). */

#ifndef _TEST_HARDWARE_CLOCKS_H_
#define _TEST_HARDWARE_CLOCKS_H_

#include <stdint.h>

enum clock_index
{
    clk_sys,
};
static inline uint32_t clock_get_hz(enum clock_index clk) { (void)clk; return 256000000; }

#endif /* _TEST_HARDWARE_CLOCKS_H_ */
