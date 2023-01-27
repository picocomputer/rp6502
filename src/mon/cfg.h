/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CFG_H_
#define _CFG_H_

#include <stdint.h>

void cfg_load();
void cfg_set_phi2_khz(uint32_t freq_khz);
uint32_t cfg_get_phi2_khz();
void cfg_set_reset_ms(uint8_t ms);
uint8_t cfg_get_reset_ms();
void cfg_set_caps(uint8_t mode);
uint8_t cfg_get_caps();

#endif /* _CFG_H_ */
