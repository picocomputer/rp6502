/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_CPU_H_
#define _RIA_SYS_CPU_H_

/* Driver for the 6502. This header is the contract shared with the
 * emulator; the hardware-only surface is in sys/cpu_hw.h.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// We run the RP2350 at 256MHz with 0.05V boost.
// One user tested up to 280 MHz on the default 1.10V.
// https://forums.raspberrypi.com/viewtopic.php?t=375975
#define CPU_RP2350_KHZ 256000

#define CPU_PHI2_MIN_KHZ 100
#define CPU_PHI2_MAX_KHZ 8000
#define CPU_PHI2_DEFAULT 8000

void cpu_init(void);

// True between cpu_run() and cpu_stop();
// the 6502 is running or about to run once RESB rises.
bool cpu_active(void);

// PHI2 without saving to config
void cpu_set_phi2_khz_run(uint16_t phi2_khz);
uint16_t cpu_get_phi2_khz_run(void);

#endif /* _RIA_SYS_CPU_H_ */
