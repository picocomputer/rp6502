/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_HID_H_
#define _RIA_USB_HID_H_

/* USB Human Interface Device
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// The various HID and HID-like systems each have their own numbering for
// managing connections. We unify these indexes into assigned "slots".
#define HID_USB_START (0x00000)
#define HID_XIN_START (0x10000)
#define HID_BLE_START (0x20000)

/* Main events
 */

void hid_task(void);

// For monitor status command.
void hid_print_status(void);
int hid_pad_count(void);

// Sends LED info to keyboards
void hid_set_leds(uint8_t leds);

#endif /* _RIA_USB_HID_H_ */
