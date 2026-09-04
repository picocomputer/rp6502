/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_GAMEPAD_H_
#define _CORE_HID_GAMEPAD_H_

/* HID Gamepad driver
 */

#include <stddef.h>
#include <stdint.h>

#include "core/hid/hid.h"
#include <stdbool.h>

/* Main events
 */

#define GAMEPAD_MAX_PLAYERS 4

// Room for button0 and button1 plus a dpad if needed.
#define GAMEPAD_MAX_BUTTONS 20

#define GAMEPAD_HOME_BUTTON 12

// The dpad byte's feature bits, ready to OR into a report.
#define GAMEPAD_FEAT_TYPE(type) ((uint8_t)((type) << 4))
#define GAMEPAD_FEAT_TYPE_MASK 0x30
#define GAMEPAD_FEAT_STICKS 0x40
#define GAMEPAD_FEAT_CONNECTED 0x80

// LED type for player indicators
enum
{
    GAMEPAD_LED_NONE,
    GAMEPAD_LED_DS4,
    GAMEPAD_LED_DS5,
};

/* One gamepad, as the driver reads it. A descriptor is parsed into this;
 * a machine with no descriptor to parse writes one out -- see the Sony
 * controllers in gamepad.c, whose own descriptors lie. */
// Gamepad descriptors are normalized to this structure.
typedef struct gamepad_connection
{
    bool valid;
    uint8_t features;  // The dpad byte's GAMEPAD_FEAT_ bits, precomputed at mount
    bool home_pressed; // Used to inject the out of band home button on xbox one
    int slot;          // HID protocol drivers use slots assigned in hid.h
    uint8_t led_type;  // GAMEPAD_LED_NONE, GAMEPAD_LED_DS4, GAMEPAD_LED_DS5
    uint8_t report_id; // If non zero, the first report byte must match and will be skipped
    bool x_absolute;   // Will be true for gamepads
    uint16_t x_offset; // Left stick X
    uint8_t x_size;
    int32_t x_min;
    int32_t x_max;
    uint16_t y_offset; // Left stick Y
    uint8_t y_size;
    int32_t y_min;
    int32_t y_max;
    uint16_t z_offset; // Right stick X (Z axis)
    uint8_t z_size;
    int32_t z_min;
    int32_t z_max;
    uint16_t rz_offset; // Right stick Y (Rz axis)
    uint8_t rz_size;
    int32_t rz_min;
    int32_t rz_max;
    uint16_t rx_offset; // Left trigger (Rx axis)
    uint8_t rx_size;
    int32_t rx_min;
    int32_t rx_max;
    uint16_t ry_offset; // Right trigger (Ry axis)
    uint8_t ry_size;
    int32_t ry_min;
    int32_t ry_max;
    uint16_t hat_offset; // D-pad/hat
    uint8_t hat_size;
    int32_t hat_min;
    int32_t hat_max;
    // Button bit offsets, 0xFFFF = unused
    uint16_t button_offsets[GAMEPAD_MAX_BUTTONS];
} gamepad_connection_t;

void gamepad_init(void);
void gamepad_stop(void);

// Where the face button labels sit, reported in dpad bits 4-5. A HID
// descriptor can't express this, so the transport says what it knows and
// nothing is claimed unless it is certain.
#define GAMEPAD_TYPE_UNKNOWN 0
#define GAMEPAD_TYPE_WESTERN 1     // A south, B east
#define GAMEPAD_TYPE_EASTERN 2     // B south, A east
#define GAMEPAD_TYPE_PLAYSTATION 3 // Cross south, Circle east

// Set the extended register value.
bool gamepad_xreg(uint16_t word);

// Whether a program has asked for this device's block.
bool gamepad_is_mapped(void);

#define GAMEPAD_PLAYERS 4

/* A flat button id spanning the report's dpad/button0/button1 fields, for a
 * host that decodes its own controller a button at a time. */
typedef enum
{
    GAMEPAD_BTN_DPAD_UP,
    GAMEPAD_BTN_DPAD_DOWN,
    GAMEPAD_BTN_DPAD_LEFT,
    GAMEPAD_BTN_DPAD_RIGHT,
    GAMEPAD_BTN_A,
    GAMEPAD_BTN_B,
    GAMEPAD_BTN_C,
    GAMEPAD_BTN_X,
    GAMEPAD_BTN_Y,
    GAMEPAD_BTN_Z,
    GAMEPAD_BTN_L1,
    GAMEPAD_BTN_R1,
    GAMEPAD_BTN_L2,
    GAMEPAD_BTN_R2,
    GAMEPAD_BTN_SELECT,
    GAMEPAD_BTN_START,
    GAMEPAD_BTN_HOME,
    GAMEPAD_BTN_L3,
    GAMEPAD_BTN_R3,
} gamepad_button_t;

/* Plug or unplug a controller a host decodes for itself. Unplugging blanks
 * the record; the type labels its face buttons and sticks says it has two. */
void gamepad_connect(int player, bool connected, uint8_t type, bool sticks);

// One button of such a controller.
void gamepad_hid_set(int player, gamepad_button_t button, bool down);

/* A whole report from one, in the block's own units: signed sticks, unsigned
 * triggers, and the dpad and button bytes as the block carries them. */
void gamepad_host_report(int player, uint8_t dpad, uint8_t button0, uint8_t button1,
                         int lx, int ly, int rx, int ry, int lt, int rt);

/* Set or clear one button in a report a host is assembling field by field,
 * which is how the desktop backends read their controllers. */
void gamepad_button_apply(gamepad_button_t button, bool down,
                          uint8_t *dpad, uint8_t *button0, uint8_t *button1);

// Parse HID report descriptor for gamepad. Devices recognized by vendor and
// product id label themselves and ignore button_type.
bool gamepad_mount(int slot, const gamepad_connection_t *desc,
                   uint16_t vendor_id, uint16_t product_id, uint8_t button_type);

// Clean up descriptor when device is disconnected.
bool gamepad_umount(int slot);

// Process HID gamepad report.
void gamepad_report(int slot, uint8_t const *data, uint16_t len);

// For xbox one, this doesn't come in reports.
void gamepad_home_button(int slot, bool pressed);

// Drivers may set on gamepad for display
int gamepad_get_player_num(int slot);

// Minimum buffer size for gamepad_build_led_report().
#define GAMEPAD_LED_REPORT_MAX 47

// Build LED output report for player indicator on Sony controllers.
// Writes into buf which must be GAMEPAD_LED_REPORT_MAX bytes.
// Sets report_id and report_len. Returns true if a LED report was written.
bool gamepad_build_led_report(int slot, uint8_t buf[GAMEPAD_LED_REPORT_MAX],
                              uint8_t *report_id, uint16_t *report_len);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define GAMEPAD_DRIVER DRIVER(gamepad_init, nul_task, nul_task, nul_run, gamepad_stop, nul_break, nul_config, nul_config)

#endif /* _CORE_HID_GAMEPAD_H_ */
