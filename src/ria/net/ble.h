/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_BLE_H_
#define _RIA_NET_BLE_H_

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

// 0-disabled, 1-enabled, 2-pairing
void ble_set_config(uint8_t ble);

// True when new devices allowed to pair
bool ble_is_pairing(void);

// Sends LED info to keyboards
void ble_set_hid_leds(uint8_t leds);

// Turn off BLE, will restart if not disabled
void ble_shutdown(void);

// Status command printer
void ble_print_status(void);

#endif /* _RIA_NET_BLE_H_ */
