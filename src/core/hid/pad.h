/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_PAD_H_
#define _CORE_HID_PAD_H_

/* HID Gamepad driver
 */

#include <stddef.h>
#include <stdint.h>

#include "core/hid/hid.h"
#include <stdbool.h>

/* Main events
 */

void pad_init(void);
void pad_stop(void);

// Where the face button labels sit, reported in dpad bits 4-5. A HID
// descriptor can't express this, so the transport says what it knows and
// nothing is claimed unless it is certain.
#define PAD_TYPE_UNKNOWN 0
#define PAD_TYPE_WESTERN 1     // A south, B east
#define PAD_TYPE_EASTERN 2     // B south, A east
#define PAD_TYPE_PLAYSTATION 3 // Cross south, Circle east

// Set the extended register value.
bool pad_xreg(uint16_t word);

// Parse HID report descriptor for gamepad. Devices recognized by vendor and
// product id label themselves and ignore button_type.
bool pad_mount(int slot, const hid_report_map_t *map,
               uint16_t vendor_id, uint16_t product_id, uint8_t button_type);

// Clean up descriptor when device is disconnected.
bool pad_umount(int slot);

// Process HID gamepad report.
void pad_report(int slot, uint8_t const *data, uint16_t len);

// For xbox one, this doesn't come in reports.
void pad_home_button(int slot, bool pressed);

// Drivers may set on gamepad for display
int pad_get_player_num(int slot);

// Minimum buffer size for pad_build_led_report().
#define PAD_LED_REPORT_MAX 47

// Build LED output report for player indicator on Sony controllers.
// Writes into buf which must be PAD_LED_REPORT_MAX bytes.
// Sets report_id and report_len. Returns true if a LED report was written.
bool pad_build_led_report(int slot, uint8_t buf[PAD_LED_REPORT_MAX],
                          uint8_t *report_id, uint16_t *report_len);

#endif /* _CORE_HID_PAD_H_ */
