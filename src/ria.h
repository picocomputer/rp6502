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

// 64KB Virtual RAM
#ifdef NDEBUG
extern uint8_t vram[0xFFFF];
#else
extern uint8_t *const vram;
#endif

void ria_stdio_init();
void ria_init();
void ria_task();
bool ria_is_active();
bool ria_set_phi2_khz(uint32_t freq_khz);
uint32_t ria_get_phi2_khz();
void ria_set_reset_ms(uint8_t ms);
uint8_t ria_get_reset_ms();
void ria_halt();
void ria_reset();
void ria_ram_read(uint32_t addr, uint8_t *buf, size_t len);

#endif /* _RIA_H_ */
