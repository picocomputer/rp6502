/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _HOST_EMU_SYS_H_
#define _HOST_EMU_SYS_H_

#include <stdint.h>

#include "core/sys.h" /* SYS_RP2350_KHZ */

/* The RIA's PIO clock divider is 16.8 fixed point, so a PHI2 period is
 * 32*int + frac/8 system ticks — not an integer. Counting the system clock in
 * eighths makes every achievable PHI2 exact: 2048 MHz, no accumulated rounding. */
#define SYS_OVERSAMPLE 8
#define SYS_TICKS_PER_US (SYS_RP2350_KHZ * SYS_OVERSAMPLE / 1000) /* 2048 */


/* The oversampled system clock; host_clock_us divides it by SYS_TICKS_PER_US to
 * serve the machine's microsecond clock. Run time is a reproducible function of
 * the frame count because the run loop advances this from an absolute per-scanline
 * deadline, never from the host's clock. */
uint64_t sys_clk_now(void);


/* One pass of this machine's super-loop: the driver walks, then any run or
 * stop that was asked for. Every host loops on this and nothing else; they
 * differ only in why they stop looping. */
void main_task(void);

#endif /* _HOST_EMU_SYS_H_ */
