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
void cfg_set_boot(char *rom);
char *cfg_get_boot(void);
bool cfg_set_codepage(uint32_t cp);
uint16_t cfg_get_codepage(void);
bool cfg_set_vga(uint8_t disp);
uint8_t cfg_get_vga(void);
bool cfg_set_rf(uint8_t rf);
uint8_t cfg_get_rf(void);
bool cfg_set_rfcc(const char *rfcc);
const char *cfg_get_rfcc(void);
bool cfg_set_ssid(const char *ssid);
const char *cfg_get_ssid(void);
bool cfg_set_pass(const char *pass);
const char *cfg_get_pass(void);
bool cfg_set_time_zone(const char *pass);
const char *cfg_get_time_zone(void);
bool cfg_set_bt(uint8_t bt);
uint8_t cfg_get_bt(void);

#endif /* _CFG_H_ */
