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

#define USB_DESC_STRING_BUF_SIZE 64
#define USB_DESC_STRING_MAX_CHAR_LEN ((USB_DESC_STRING_BUF_SIZE - 2) / 2)

uint16_t usb_desc_string_ulen(const void *desc_buf, size_t desc_buf_size);

void usb_desc_string_to_oem(const void *desc_buf, size_t desc_buf_size, char *dest, size_t dest_size);

// These fetches call sys_task while they wait, for up to 250 ms. Each returns
// the same static buffer of USB_DESC_STRING_BUF_SIZE bytes, so a result must
// be used before the next fetch. A fetch returns NULL when it cannot start or
// does not finish within 250 ms, and returns a zeroed buffer when the device
// has no such string or the transfer fails.
const void *usb_string_fetch_manufacturer(uint8_t daddr);
const void *usb_string_fetch_product(uint8_t daddr);
const void *usb_string_fetch_serial(uint8_t daddr);

bool usb_device_id_hash(uint8_t daddr, char *buf, size_t buf_size);

/* USB_DRIVER comes late in the driver list because usb_init starts the 355 ms
 * boot enumeration window, and keyboard_mount applies the NumLock quirk of the
 * Raspberry Pi Keyboard only inside that window. A slow init after usb_init
 * can use up the window before the keyboard mounts. */
#define USB_DRIVER DRIVER(usb_init, usb_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _RIA_USB_USB_H_ */
