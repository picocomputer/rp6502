/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_MAIN_H_
#define _FPGA_SW_MAIN_H_

void main_restored(void);
void main_wake_failed(void);

#include <stdint.h>
#include <stdbool.h>

extern bool main_boot_wake;
extern uint32_t main_boot_slot;
extern uint8_t main_boot_upd;

bool main_xreg_0(uint8_t channel, uint8_t address, uint16_t word);
bool main_xreg_1(uint8_t channel, uint8_t address, uint16_t word);

void tim_set_tz_minutes(int32_t min);

#endif
