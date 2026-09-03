/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_W_BLE_BLE_H_
#define _RIA_W_BLE_BLE_H_

/* Bluetooth LE driver, main events and HID.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void ble_task(void);

/* Utility
 */

// True when new devices allowed to pair
bool ble_is_pairing(void);

// Sends LED info to keyboards
void ble_set_hid_leds(uint8_t leds);

// Turn off BLE, will restart if not disabled
void ble_shutdown(void);

// Status command printer
int ble_status_response(char *buf, size_t buf_size, int state, unsigned width);

// Configuration setting BLE
void ble_init(void);
bool ble_check_enabled(uint8_t *v);
void ble_apply_enabled(uint8_t ble, bool changed);
int ble_enabled_response(char *buf, size_t buf_size, int state, unsigned width);

/* This driver's row in a machine's driver list; see core/sys/driver.h. The radio's other half. Its shutdown is cyw's to order, not the walk's
 * -- see cyw_reset_radio. */
#define BLE_CONFIG_ENABLED CONFIG_INT(B, ble, enabled, uint8_t, 1, \
    ble_check_enabled, ble_apply_enabled, STR_BLE, ble_enabled_response, \
    STR_HELP_SET_BLE, NULL)
#define BLE_DRIVER DRIVER(ble_init, ble_task, nul_task, nul_run, nul_stop, nul_break, \
    BLE_CONFIG_ENABLED, nul_config)

#endif /* _RIA_W_BLE_BLE_H_ */
