/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CFG_H_
#define _CFG_H_

#include <stdint.h>
#include <stdbool.h>

/* Kernel events
 */

void cfg_init(void);

// These setters will auto save on change and
// reconfigure the system as necessary.

bool cfg_set_phi2_khz(uint32_t freq_khz);
uint32_t cfg_get_phi2_khz(void);
void cfg_set_reset_ms(uint8_t ms);
uint8_t cfg_get_reset_ms(void);
void cfg_set_caps(uint8_t mode);
uint8_t cfg_get_caps(void);
void cfg_set_boot(char *rom);
char *cfg_get_boot(void);
bool cfg_set_codepage(uint32_t cp);
uint16_t cfg_get_codepage(void);
bool cfg_set_vga(uint8_t disp);
uint8_t cfg_get_vga(void);

#endif /* _CFG_H_ */
