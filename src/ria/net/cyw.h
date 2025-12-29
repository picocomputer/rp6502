/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_CYW_H_
#define _RIA_NET_CYW_H_

/* Device driver for the CYW43 radio module.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void cyw_init(void);
void cyw_task(void);

/* Utility
 */

// Pico W has LED on a CYW gpio
void cyw_led_set(bool on);

// Configuration setting RF
void cyw_load_rf_enable(const char *str, size_t len);
bool cyw_set_rf_enable(uint8_t rf);
uint8_t cyw_get_rf_enable(void);

// Configuration setting RFCC
void cyw_load_rf_country_code(const char *str, size_t len);
bool cyw_set_rf_country_code(const char *rfcc);
const char *cyw_get_rf_country_code(void);
const char *cyw_get_rf_country_code_verbose(void);

// List known country codes for help
int cyw_country_code_response(char *buf, size_t buf_size, int state);

#endif /* _RIA_NET_CYW_H_ */
