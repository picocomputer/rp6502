/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_ACTION_H_
#define _RIA_ACTION_H_

#include <stdbool.h>

void ria_action_loop();
void ria_action_task();
void ria_action_reset();
bool ria_action_in_progress();
void ria_action_pio_init();
int32_t ria_action_result();
void ria_action_ram_read(uint16_t addr, uint8_t *buf, uint16_t len);
void ria_action_ram_write(uint16_t addr, const uint8_t *buf, uint16_t len);
void ria_action_ram_verify(uint16_t addr, const uint8_t *buf, uint16_t len);
void ria_action_jmp(uint16_t addr);

#endif /* _RIA_ACTION_H_ */
