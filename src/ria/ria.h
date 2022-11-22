/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_H_
#define _RIA_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void ria_stdio_init();
void ria_stdio_flush();
void ria_init();
void ria_task();
bool ria_is_active();
void ria_set_phi2_khz(uint32_t freq_khz);
uint32_t ria_get_phi2_khz();
void ria_set_reset_ms(uint8_t ms);
uint8_t ria_get_reset_ms();
uint32_t ria_get_reset_us();
void ria_set_caps(uint8_t mode);
uint8_t ria_get_caps();
void ria_halt();
void ria_reset();
void ria_ram_write(uint32_t addr, uint8_t *buf, size_t len);
void ria_ram_read(uint32_t addr, uint8_t *buf, size_t len);
void ria_jmp(uint32_t addr);

#endif /* _RIA_H_ */
