/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_BLE_BLE_H_
#define _RIA_BLE_BLE_H_

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
void ble_load_enabled(const char *str);
bool ble_set_enabled(unsigned ble);
uint8_t ble_get_enabled(void);

/* This driver's row in a machine's driver list; see core/mach.h. The radio's other half. Its shutdown is cyw's to order, not the walk's
 * -- see cyw_reset_radio. */
#define BLE_DRIVER DRIVER(nul_init, ble_task, nul_task, nul_run, nul_stop, nul_break)

#endif /* _RIA_BLE_BLE_H_ */
