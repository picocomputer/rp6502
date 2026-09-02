/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_CPU_H_
#define _CORE_CPU_H_

#include <stdbool.h>
#include <stdint.h>

#define CPU_PHI2_MIN_KHZ 100
#define CPU_PHI2_MAX_KHZ 8000
#define CPU_PHI2_DEFAULT 8000

// True between cpu_run() and cpu_stop();
// the 6502 is running or about to run once RESB rises.
bool cpu_active(void);
void cpu_stop(void);

/* Setter may asjust to nearby rate; the getter reports the one chosen. */
uint16_t cpu_get_phi2_khz_run(void);
void cpu_set_phi2_khz_run(uint16_t phi2_khz);

#endif /* _CORE_CPU_H_ */
