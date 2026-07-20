/*
 * Copyright (c) 2026 Rumbledethumps
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

#define CPU_PHI2_MIN_KHZ 100
#define CPU_PHI2_MAX_KHZ 8000
#define CPU_PHI2_DEFAULT 8000

/* Main events
 */

void cpu_main(void);
void cpu_init(void);
void cpu_task(void);
void cpu_run(void);
void cpu_stop(void);
void cpu_reclock(void);

// True between cpu_run() and cpu_stop();
// the 6502 is running or about to run once RESB rises.
bool cpu_active(void);

// Return calculated reset time. May be higher than configured
// to guarantee the 6502 gets two clock cycles during reset.
uint32_t cpu_get_reset_us(void);

// PHI2 without saving to config
void cpu_set_phi2_khz_run(uint16_t phi2_khz);
uint16_t cpu_get_phi2_khz_run(void);

// Configuration setting PHI2
void cpu_load_phi2_khz(const char *str);
bool cpu_set_phi2_khz(uint16_t phi2_khz);
uint16_t cpu_get_phi2_khz(void);

#endif /* _RIA_SYS_CPU_H_ */
