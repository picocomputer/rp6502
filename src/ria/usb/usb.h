/*
 * Copyright (c) 2026 Rumbledethumps
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
int usb_status_response(char *buf, size_t buf_size, int state, unsigned width);

// Sends LED info to keyboards
void usb_set_hid_leds(uint8_t leds);

// True when USB in boot enumeration sequence
bool usb_boot_enumerating(void);

/* USB string descriptors
 */

// Fetch/conversion buffer: 2-byte header + 31 UTF-16 chars
#define USB_DESC_STRING_BUF_SIZE 64
#define USB_DESC_STRING_MAX_CHAR_LEN ((USB_DESC_STRING_BUF_SIZE - 2) / 2)

// UTF-16 char count in a string descriptor, clamped to the buffer capacity.
uint16_t usb_desc_string_ulen(const void *desc_buf, size_t desc_buf_size);

// Convert USB string descriptor to OEM for display.
void usb_desc_string_to_oem(const void *desc_buf, size_t desc_buf_size, char *dest, size_t dest_size);

// Blocking fetches returning a shared USB_DESC_STRING_BUF_SIZE buffer.
const void *usb_string_fetch_manufacturer(uint8_t daddr);
const void *usb_string_fetch_product(uint8_t daddr);
const void *usb_string_fetch_serial(uint8_t daddr);

// Stable device identity fingerprint (VID/PID/bcdDevice + descriptor strings)
bool usb_device_id_hash(uint8_t daddr, char *buf, size_t buf_size);

#endif /* _RIA_USB_USB_H_ */
