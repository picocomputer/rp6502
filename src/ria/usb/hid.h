/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HID_H_
#define _HID_H_

#include "tusb_config.h"
#include "btstack_config.h"

// The various HID and HID-like systems each have their own numbering for
// managing connections. We unify these indexes into assigned "slots".
#define HID_USB_START (0x00000)
#define HID_XIN_START (0x10000)
#define HID_BLE_START (0x20000)

void hid_task(void);
void hid_print_status(void);
void hid_set_leds(uint8_t leds);
int hid_pad_count(void);

#endif /* _HID_H_ */
