/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_W_NET_CYW_H_
#define _RIA_W_NET_CYW_H_

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

#define CYW_COUNTRY_CODE_SIZE 4

// Configuration setting RF
bool cyw_check_rf_enable(uint8_t *v);
void cyw_apply_rf_enable(uint8_t rf, bool changed);
int cyw_rf_enable_response(char *buf, size_t buf_size, int state, unsigned width);


// Configuration setting RFCC
bool cyw_check_rf_country_code(const char *in, char *out);
void cyw_apply_rf_country_code(const char *rfcc, bool changed);
int cyw_rf_country_code_response(char *buf, size_t buf_size, int state, unsigned width);
const char *cyw_get_rf_country_code_verbose(void);

// List known country codes for help
int cyw_country_code_response(char *buf, size_t buf_size, int state, unsigned width);

/* Hardware, but after CFG in the driver list rather than among the bring-up rows
 * ahead of it: the country code is an argument to the radio's bring-up call,
 * so this cannot precede the config load. */
#define CYW_CONFIG_RF CONFIG_INT(E, cyw, rf_enable, uint8_t, 1, \
    cyw_check_rf_enable, cyw_apply_rf_enable, STR_RF, cyw_rf_enable_response, \
    STR_HELP_SET_RF, NULL)
#define CYW_CONFIG_RFCC CONFIG_STR(F, cyw, rf_country_code, CYW_COUNTRY_CODE_SIZE, "", \
    cyw_check_rf_country_code, cyw_apply_rf_country_code, STR_RFCC, \
    cyw_rf_country_code_response, STR_HELP_SET_RFCC, cyw_country_code_response)
#define CYW_DRIVER DRIVER(cyw_init, cyw_task, nul_task, nul_run, nul_stop, nul_break, \
    CYW_CONFIG_RF, CYW_CONFIG_RFCC)

#endif /* _RIA_W_NET_CYW_H_ */
