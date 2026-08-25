/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The 6502 every machine runs -- a real W65C02S clocked by PIO on the Pico, a
 * cycle model in the emulator, the fabric's on a Pocket. The pins and the
 * lifecycle belong to whoever wires it. */

#ifndef _CORE_CPU_H_
#define _CORE_CPU_H_

#include <stdbool.h>
#include <stdint.h>

/* The API's range, not any one platform's: a program asking for 20 MHz is out
 * of range everywhere. */
#define CPU_PHI2_MIN_KHZ 100
#define CPU_PHI2_MAX_KHZ 8000
#define CPU_PHI2_DEFAULT 8000

// True between cpu_run() and cpu_stop();
// the 6502 is running or about to run once RESB rises.
bool cpu_active(void);

/* In kHz. The setter may land on a nearby achievable rate; the getter reports
 * the one chosen. */
uint16_t cpu_get_phi2_khz_run(void);
void cpu_set_phi2_khz_run(uint16_t phi2_khz);

#endif /* _CORE_CPU_H_ */
