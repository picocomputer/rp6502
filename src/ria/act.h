/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _ACT_H_
#define _ACT_H_

#include <stdbool.h>

void act_loop();
void act_task();
void act_reset();
bool act_in_progress();
void act_pio_init();
int32_t act_result();
void act_ram_read(uint16_t addr, uint8_t *buf, uint16_t len);
void act_ram_write(uint16_t addr, const uint8_t *buf, uint16_t len);
void act_ram_verify(uint16_t addr, const uint8_t *buf, uint16_t len);

#endif /* _ACT_H_ */
