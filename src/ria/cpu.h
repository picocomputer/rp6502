/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CPU_H_
#define _CPU_H_

#include <stdint.h>
#include <stdbool.h>

void cpu_run();
void cpu_stop();
void cpu_init();
void cpu_task();
bool cpu_is_active();
void cpu_api_phi2();

uint32_t cpu_validate_phi2_khz(uint32_t freq_khz);
bool cpu_set_phi2_khz(uint32_t freq_khz);
uint32_t cpu_get_reset_us();

#endif /* _CPU_H_ */