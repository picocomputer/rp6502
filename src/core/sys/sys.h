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

/* That clock is what host_clock_us divides down, and the only thing that
 * advances it is the CPU catching up to the beam -- an absolute per-scanline
 * deadline, never the host's clock. So run time is a reproducible function of
 * the frames that went by, which is what makes a timed test repeat. */

/* One pass of this machine's super-loop: the driver walks, then any run or
 * stop that was asked for. Every host loops on this and nothing else; they
 * differ only in why they stop looping. */
void main_task(void);

#endif /* _HOST_EMU_SYS_H_ */
