/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_GAMEPAD_H_
#define _CORE_HID_GAMEPAD_H_

#include <stddef.h>
#include <stdint.h>

#include "core/hid/hid.h"
#include "core/sys/sst.h"
#include <stdbool.h>

#define GAMEPAD_MAX_PLAYERS 4

// Eight bits of button0, eight of button1, and four discrete dpad buttons.
#define GAMEPAD_MAX_BUTTONS 20

#define GAMEPAD_HOME_BUTTON 12

#define GAMEPAD_FEAT_TYPE(type) ((uint8_t)((type) << 4))
#define GAMEPAD_FEAT_TYPE_MASK 0x30
#define GAMEPAD_FEAT_STICKS 0x40
#define GAMEPAD_FEAT_CONNECTED 0x80

enum
{
    GAMEPAD_LED_NONE,
    GAMEPAD_LED_DS4,
    GAMEPAD_LED_DS5,
};

typedef struct gamepad_connection
{
    bool valid;
    uint8_t features;  // the dpad byte's GAMEPAD_FEAT_ bits
    bool home_pressed; // an Xbox One reports home outside its report
    int slot;
    uint8_t led_type;
    uint8_t report_id; // when non-zero, the report starts with this byte
    bool x_absolute;
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

// Which labels the face buttons carry, reported in dpad bits 4 and 5. A HID
// descriptor cannot express this, so the transport says what it knows and a
// device it does not recognize stays GAMEPAD_TYPE_UNKNOWN.
#define GAMEPAD_TYPE_UNKNOWN 0
#define GAMEPAD_TYPE_WESTERN 1     // A south, B east
#define GAMEPAD_TYPE_EASTERN 2     // B south, A east
#define GAMEPAD_TYPE_PLAYSTATION 3 // Cross south, Circle east

bool gamepad_xreg(uint16_t word);

bool gamepad_is_mapped(void);

#define GAMEPAD_PLAYERS 4

/* A button id spanning the report's dpad, button0 and button1 fields. */
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

/* Plug or unplug a controller a host decodes for itself. Unplugging blanks the
 * report. type is a GAMEPAD_TYPE_ and sticks says both analog sticks are
 * there. */
void gamepad_connect(int player, bool connected, uint8_t type, bool sticks);

void gamepad_hid_set(int player, gamepad_button_t button, bool down);

/* A whole report in the block's units: signed sticks, unsigned triggers. */
void gamepad_host_report(int player, uint8_t dpad, uint8_t button0, uint8_t button1,
                         int lx, int ly, int rx, int ry, int lt, int rt);

void gamepad_button_apply(gamepad_button_t button, bool down,
                          uint8_t *dpad, uint8_t *button0, uint8_t *button1);

// A device whose vendor and product ids give it a type labels its own face
// buttons and ignores button_type.
bool gamepad_mount(int slot, const gamepad_connection_t *desc,
                   uint16_t vendor_id, uint16_t product_id, uint8_t button_type);

bool gamepad_umount(int slot);

void gamepad_report(int slot, uint8_t const *data, uint16_t len);

// An Xbox One sends the home button outside its reports.
void gamepad_home_button(int slot, bool pressed);

int gamepad_get_player_num(int slot);

#define GAMEPAD_LED_REPORT_MAX 47

// The player indicator on a Sony controller; false for any other device.
bool gamepad_build_led_report(int slot, uint8_t buf[GAMEPAD_LED_REPORT_MAX],
                              uint8_t *report_id, uint16_t *report_len);

/* The blob carries where the program asked for the four players' reports and
 * the reports themselves: 2 + 4 * 10. A report stands on its own, because a
 * host that decodes its own controller writes the feature bits straight into
 * one rather than keeping a connection to read them from. */
#define GAMEPAD_SST_SIZE 42
void gamepad_sst_save(sst_cursor_t *c, unsigned flags);
bool gamepad_sst_load(sst_cursor_t *c, unsigned flags);

#define GAMEPAD_DRIVER DRIVER(gamepad_init, nul_task, nul_task, nul_run, gamepad_stop, nul_break, \
    nul_config, nul_config, SST(GPAD, 1, GAMEPAD_SST_SIZE, gamepad_sst_save, gamepad_sst_load))

#endif /* _CORE_HID_GAMEPAD_H_ */
