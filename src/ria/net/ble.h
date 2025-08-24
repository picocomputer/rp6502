/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_BLE_H_
#define _RIA_NET_BLE_H_

/* Bluetooth LE driver
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Kernel events
 */

void ble_task(void);

/* Utility
 */

void ble_set_config(uint8_t bt);

// True when new devices allowed to pair
bool ble_is_pairing(void);

// Sends LED info to keyboards
void ble_set_leds(uint8_t leds);

void ble_shutdown(void);

void ble_print_status(void);

#endif /* _RIA_NET_BLE_H_ */
