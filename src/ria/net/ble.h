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

// True when new devices allowed to pair
bool ble_is_pairing(void);

// Sends LED info to keyboards
void ble_set_hid_leds(uint8_t leds);

// Turn off BLE, will restart if not disabled
void ble_shutdown(void);

// Status command printer
int ble_status_response(char *buf, size_t buf_size, int state);

// Configuration setting BLE
void ble_load_enabled(const char *str, size_t len);
bool ble_set_enabled(uint8_t bt);
uint8_t ble_get_enabled(void);

#endif /* _RIA_NET_BLE_H_ */
