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
#include "core/cpu.h"

#include <stdint.h>
#include <stdbool.h>

#define CPU_RESB_PIN 26
#define CPU_IRQB_PIN 22
#define CPU_PHI2_PIN 21

/* Main events
 */
void cpu_init(void);
void cpu_task(void);
void cpu_run(void);
void cpu_stop(void);
void cpu_reclock(void);

// Return calculated reset time. May be higher than configured
// to guarantee the 6502 gets two clock cycles during reset.
uint32_t cpu_get_reset_us(void);

// Configuration setting PHI2 (the pair it validates into is core/cpu.h's)
void cpu_load_phi2_khz(const char *str);

/* This driver's row in a machine's driver list; see core/mach.h. A row lives with the
 * implementation, not the contract: which hooks a machine's CPU has is the
 * implementation's answer, and three of them differ. */
#define CPU_DRIVER DRIVER(cpu_init, cpu_task, nul_task, cpu_run, cpu_stop, nul_break)

#endif /* _RIA_SYS_CPU_H_ */
