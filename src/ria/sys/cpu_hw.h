/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_CPU_HW_H_
#define _RIA_SYS_CPU_HW_H_

/* Driver for the 6502, hardware-only surface: pins, vreg, main
 * events, and the config-facing PHI2 setters.
 */

#include "sys/cpu.h"

#define CPU_RP2350_VREG VREG_VOLTAGE_1_15

#define CPU_RESB_PIN 26
#define CPU_IRQB_PIN 22
#define CPU_PHI2_PIN 21

/* Main events
 */

void cpu_main(void);
void cpu_task(void);
void cpu_run(void);
void cpu_stop(void);
void cpu_reclock(void);

// Return calculated reset time. May be higher than configured
// to guarantee the 6502 gets two clock cycles during reset.
uint32_t cpu_get_reset_us(void);

// Configuration setting PHI2
void cpu_load_phi2_khz(const char *str);
bool cpu_set_phi2_khz(uint16_t phi2_khz);
uint16_t cpu_get_phi2_khz(void);

#endif /* _RIA_SYS_CPU_HW_H_ */
