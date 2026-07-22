/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_SYS_SYS_H_
#define _EMU_SYS_SYS_H_

#include <stdint.h>

/* The firmware contract: sys_init (the monitor's startup banner) and sys_main (the
 * RP2350 clock and voltage), neither of which the emulator implements. SYS_RP2350_KHZ
 * comes from here too. */
#include "ria/sys/sys.h"

/* The RIA's PIO clock divider is 16.8 fixed point, so a PHI2 period is
 * 32*int + frac/8 system ticks — not an integer. Counting the system clock in
 * eighths makes every achievable PHI2 exact: 2048 MHz, no accumulated rounding. */
#define SYS_OVERSAMPLE 8
#define SYS_TICKS_PER_US (SYS_RP2350_KHZ * SYS_OVERSAMPLE / 1000) /* 2048 */

/* Run one 60 Hz VGA frame. The norender form advances the CPU, chips, timing and
 * vsync but skips the per-scanline pixel work — a catch-up frame the pacer will not
 * present, and most of the per-frame cost. */
void sys_run_frame(void);
void sys_run_frame_norender(void);

/* The oversampled system clock; pico/time.h's time_us_64 divides it by
 * SYS_TICKS_PER_US to serve the pico monotonic microsecond clock. */
uint64_t sys_clk_now(void);

unsigned long sys_frame_count(void); /* diagnostic: total frames, advances at 60 Hz */

#endif /* _EMU_SYS_SYS_H_ */
