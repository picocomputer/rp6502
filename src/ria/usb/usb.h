/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_USB_H_
#define _RIA_USB_USB_H_

/* USB host driver, main events and HID.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void usb_init(void);
void usb_task(void);

// For monitor status command.
void usb_print_status(void);

// Sends LED info to keyboards
void usb_set_hid_leds(uint8_t leds);

#endif /* _RIA_USB_USB_H_ */
