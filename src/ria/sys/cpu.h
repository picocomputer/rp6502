/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CPU_H_
#define _CPU_H_

#include <stdint.h>
#include <stdbool.h>

/* Kernel events
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

uint32_t cpu_validate_phi2_khz(uint32_t freq_khz);
bool cpu_set_phi2_khz(uint32_t freq_khz);

// Return calculated reset time. May be higher than configured
// to guarantee the 6502 gets two clock cycles during reset.
uint32_t cpu_get_reset_us();

#endif /* _CPU_H_ */
