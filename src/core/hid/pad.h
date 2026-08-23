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

// Whether a program has asked for this device's block.
bool pad_is_mapped(void);

#define PAD_PLAYERS 4

/* A flat button id spanning the report's dpad/button0/button1 fields, for a
 * host that decodes its own controller a button at a time. */
typedef enum
{
    PAD_BTN_DPAD_UP,
    PAD_BTN_DPAD_DOWN,
    PAD_BTN_DPAD_LEFT,
    PAD_BTN_DPAD_RIGHT,
    PAD_BTN_A,
    PAD_BTN_B,
    PAD_BTN_C,
    PAD_BTN_X,
    PAD_BTN_Y,
    PAD_BTN_Z,
    PAD_BTN_L1,
    PAD_BTN_R1,
    PAD_BTN_L2,
    PAD_BTN_R2,
    PAD_BTN_SELECT,
    PAD_BTN_START,
    PAD_BTN_HOME,
    PAD_BTN_L3,
    PAD_BTN_R3,
} pad_button_t;

/* Plug or unplug a controller a host decodes for itself. Unplugging blanks
 * the record; the type labels its face buttons and sticks says it has two. */
void pad_connect(int player, bool connected, uint8_t type, bool sticks);

// One button of such a controller.
void pad_hid_set(int player, pad_button_t button, bool down);

/* A whole report from one, in the block's own units: signed sticks, unsigned
 * triggers, and the dpad and button bytes as the block carries them. */
void pad_host_report(int player, uint8_t dpad, uint8_t button0, uint8_t button1,
                     int lx, int ly, int rx, int ry, int lt, int rt);

/* Set or clear one button in a report a host is assembling field by field,
 * which is how the desktop backends read their controllers. */
void pad_button_apply(pad_button_t button, bool down,
                      uint8_t *dpad, uint8_t *button0, uint8_t *button1);

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
