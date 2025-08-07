/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HID_H_
#define _HID_H_

#include "tusb_config.h"
#include "btstack_config.h"

// The various HID and HID-like systems each have their own arrays for
// managing connections. We unify these indexes into assigned "slots".
#define HID_USB_START (0)
#define HID_USB_SLOTS (CFG_TUH_HID)
#define HID_XIN_START (HID_USB_START + HID_USB_SLOTS)
#define HID_XIN_SLOTS (PAD_MAX_PLAYERS)
#define HID_BLE_START (HID_XIN_START + HID_XIN_SLOTS)
#define HID_BLE_SLOTS (MAX_NR_HCI_CONNECTIONS)

void hid_task(void);
void hid_print_status(void);
void hid_set_leds(uint8_t leds);
int hid_pad_count(void);

#endif /* _HID_H_ */
