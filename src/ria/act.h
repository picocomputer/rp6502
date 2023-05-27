/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _ACT_H_
#define _ACT_H_

#define ACT_PIO pio1
#define ACT_SM 0

#include <stdbool.h>

void act_task();
bool act_error_message();
void act_run();
void act_stop();
bool act_in_progress();
void act_init();
void act_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac);
void act_ram_read(uint16_t addr);
void act_ram_write(uint16_t addr);
void act_ram_verify(uint16_t addr);

#endif /* _ACT_H_ */
