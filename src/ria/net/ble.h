/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _BLE_H_
#define _BLE_H_

#include <stdint.h>
#include <stdbool.h>

/* Kernel events
 */

void ble_task(void);

/* Utility
 */

void ble_set_config(uint8_t bt);

bool ble_is_pairing(void);

void ble_set_leds(uint8_t leds);

void ble_shutdown(void);

void ble_print_status(void);

#endif /* _BLE_H_ */
