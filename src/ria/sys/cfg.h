/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_CFG_H_
#define _CF_RIA_SYS_CFG_H_G_H_

/* System configuration manager.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void cfg_init(void);

void cfg_save(void);

// The boot string isn't stored in RAM.
void cfg_save_boot(const char *str);
const char *cfg_load_boot(void);

bool cfg_set_code_page(uint32_t cp);
uint16_t cfg_get_code_page(void);
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
bool cfg_set_ble(uint8_t bt);
uint8_t cfg_get_ble(void);

#endif /* _RIA_SYS_CFG_H_ */
