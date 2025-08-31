/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_CPU_H_
#define _RIA_SYS_CPU_H_

/* Driver for the 6502.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define CPU_RESB_PIN 26
#define CPU_IRQB_PIN 22
#define CPU_PHI2_PIN 21

#define CPU_PHI2_MIN_KHZ 800
#define CPU_PHI2_MAX_KHZ 8000
#define CPU_PHI2_DEFAULT 8000

/* Main events
 */

void cpu_init(void);
void cpu_task(void);
void cpu_run(void);
void cpu_stop(void);
void cpu_post_reclock(void);
bool cpu_api_phi2(void);

// The CPU is active when RESB is high or when
// we're waiting for the RESB timer.
bool cpu_active(void);

/* Config handlers
 */

// The will return a validated freq_khz from the
// range defined above. 0 returns the default.
uint32_t cpu_validate_phi2_khz(uint32_t freq_khz);

// This was a major engineering effort.
bool cpu_set_phi2_khz(uint32_t freq_khz);

// Return calculated reset time. May be higher than configured
// to guarantee the 6502 gets two clock cycles during reset.
uint32_t cpu_get_reset_us();

#endif /* _RIA_SYS_CPU_H_ */
