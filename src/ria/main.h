/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MAIN_H_
#define _MAIN_H_

#include <stdint.h>
#include <stdbool.h>

void main_run();
void main_stop();
void main_break();

void main_task();
void main_preclock();
void main_reclock(uint32_t phi2_khz, uint32_t sys_clk_khz, uint16_t clkdiv_int, uint8_t clkdiv_frac);
bool main_api(uint8_t operation);

#endif /* _MAIN_H_ */
