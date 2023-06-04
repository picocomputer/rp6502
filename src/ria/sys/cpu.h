/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CPU_H_
#define _CPU_H_

#include <stdint.h>
#include <stdbool.h>

#define CPU_RESB_PIN 26
#define CPU_IRQB_PIN 22
#define CPU_PHI2_PIN 21

// Short circuit this to the RIA action loop
extern volatile int cpu_rx_char;

void cpu_init();
void cpu_task();
void cpu_run();
void cpu_stop();
void cpu_reclock();
void cpu_api_phi2();
bool cpu_active();

uint32_t cpu_validate_phi2_khz(uint32_t freq_khz);
bool cpu_set_phi2_khz(uint32_t freq_khz);
uint32_t cpu_get_reset_us();
void cpu_com_rx(uint8_t ch);

#endif /* _CPU_H_ */
