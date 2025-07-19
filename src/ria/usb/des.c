/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"
#include "btstack.h"
#include "usb/des.h"
#include "usb/xin.h"
#include <string.h>

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_DES)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// If you're here to remap HID buttons on a new HID gamepad, create
// a new des_remap_ function and add it to des_report_descriptor().

static inline void swap(des_gamepad_t *gamepad, int a, int b)
{
    uint16_t temp = gamepad->button_offsets[a];
    gamepad->button_offsets[a] = gamepad->button_offsets[b];
    gamepad->button_offsets[b] = temp;
}

static void des_remap_playstation_classic(des_gamepad_t *gamepad, uint16_t vendor_id)
{
    if (vendor_id != 0x054C) // Sony Interactive Entertainment
        return;
    gamepad->sony = true;
    swap(gamepad, 0, 2); // buttons
    swap(gamepad, 2, 3); // buttons
    swap(gamepad, 4, 8); // l1/l2
    swap(gamepad, 5, 9); // r1/r2
    swap(gamepad, 4, 6); // l1/bt
    swap(gamepad, 5, 7); // r1/st
}

static void des_remap_8bitdo_dinput(des_gamepad_t *gamepad,
                                    uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id != 0x2DC8) // 8BitDo
        return;
    DBG("Remapping 8BitDo Dinput buttons.\n");
    // All 8BitDo controllers in DInput mode have "gaps" in their buttons.
    uint16_t temp2 = gamepad->button_offsets[2];
    uint16_t temp5 = gamepad->button_offsets[5];
    gamepad->button_offsets[2] = gamepad->button_offsets[3];
    gamepad->button_offsets[3] = gamepad->button_offsets[4];
    for (int i = 4; i <= 9; i++)
        gamepad->button_offsets[i] = gamepad->button_offsets[i + 2];
    gamepad->button_offsets[10] = gamepad->button_offsets[13];
    gamepad->button_offsets[11] = gamepad->button_offsets[14];
    // Swap buttons 6,7 with 8,9
    swap(gamepad, 6, 8);
    swap(gamepad, 7, 9);
    // M30 special case
    if (product_id == 0x5006)
    {
        // The home button only when wired (pid=0x5006)
        uint16_t temp12 = gamepad->button_offsets[12];
        gamepad->button_offsets[12] = temp2;
        temp2 = temp12;
    }
    // Drop the gaps at the end, not sure what uses this.
    gamepad->button_offsets[13] = temp2;
    gamepad->button_offsets[14] = temp5;
}

static const des_gamepad_t __in_flash("hid_descriptors") des_xbox_one = {
    .valid = true,
    .report_id = 0x20, // GIP message ID
    .x_offset = 9 * 8, // left stick X
    .x_size = 16,
    .x_min = -32768,
    .x_max = 32767,
    .y_offset = 11 * 8, // left stick Y
    .y_size = 16,
    .y_min = 32767,
    .y_max = -32768,
    .z_offset = 13 * 8, // right stick X
    .z_size = 16,
    .z_min = -32768,
    .z_max = 32767,
    .rz_offset = 15 * 8, // right stick Y
    .rz_size = 16,
    .rz_min = 32767,
    .rz_max = -32768,
    .rx_offset = 5 * 8, // left trigger
    .rx_size = 10,
    .rx_min = 0,
    .rx_max = 1023,
    .ry_offset = 7 * 8, // right trigger
    .ry_size = 10,
    .ry_min = 0,
    .ry_max = 1023,
    .button_offsets = {
        // Xbox One Gamepad Input Protocol buttons
        3 * 8 + 4, // A button
        3 * 8 + 5, // B button
        3 * 8 + 6, // X button
        3 * 8 + 7, // Y button
        4 * 8 + 4, // Left shoulder/LB
        4 * 8 + 5, // Right shoulder/RB
        3 * 8 + 3, // View/Select button
        3 * 8 + 2, // Menu/Start button
        //
        0xFFFF,    // L2
        0xFFFF,    // R2
        4 * 8 + 6, // Left stick click
        4 * 8 + 7, // Right stick click
        0xFFFF,    // Home button
        0xFFFF,    // unused
        0xFFFF,    // unused
        0xFFFF,    // unused
        //
        4 * 8 + 0, // D-pad Up
        4 * 8 + 1, // D-pad Down
        4 * 8 + 2, // D-pad Left
        4 * 8 + 3, // D-pad Right
    }};

static const des_gamepad_t __in_flash("hid_descriptors") des_xbox_360 = {
    .valid = true,
    .report_id = 0,    // Xbox 360 uses no report ID for input reports
    .x_offset = 6 * 8, // left stick X
    .x_size = 16,
    .x_min = -32768,
    .x_max = 32767,
    .y_offset = 8 * 8, // left stick Y
    .y_size = 16,
    .y_min = 32767,
    .y_max = -32768,
    .z_offset = 10 * 8, // right stick X
    .z_size = 16,
    .z_min = -32768,
    .z_max = 32767,
    .rz_offset = 12 * 8, // right stick Y
    .rz_size = 16,
    .rz_min = 32767,
    .rz_max = -32768,
    .rx_offset = 4 * 8, // left trigger
    .rx_size = 8,
    .rx_min = 0,
    .rx_max = 255,
    .ry_offset = 5 * 8, // right trigger
    .ry_size = 8,
    .ry_min = 0,
    .ry_max = 255,
    .button_offsets = {
        // Xbox 360 USB report button layout
        3 * 8 + 4, // A button
        3 * 8 + 5, // B button
        3 * 8 + 6, // X button
        3 * 8 + 7, // Y button
        3 * 8 + 0, // Left shoulder/LB
        3 * 8 + 1, // Right shoulder/RB
        2 * 8 + 5, // Back button
        2 * 8 + 4, // Start button
        //
        0xFFFF,    // L2
        0xFFFF,    // R2
        2 * 8 + 6, // Left stick click
        2 * 8 + 7, // Right stick click
        3 * 8 + 2, // Home button
        0xFFFF,    // unused
        0xFFFF,    // unused
        0xFFFF,    // unused
        //
        2 * 8 + 0, // D-pad Up
        2 * 8 + 1, // D-pad Down
        2 * 8 + 2, // D-pad Left
        2 * 8 + 3  // D-pad Right
    }};

static const des_gamepad_t __in_flash("hid_descriptors") des_sony_ds4 = {
    .valid = true,
    .sony = true,
    .report_id = 1,
    .x_offset = 0 * 8, // left stick X
    .x_size = 8,
    .x_min = 0,
    .x_max = 255,
    .y_offset = 1 * 8, // left stick Y
    .y_size = 8,
    .y_min = 0,
    .y_max = 255,
    .z_offset = 2 * 8, // right stick X
    .z_size = 8,
    .z_min = 0,
    .z_max = 255,
    .rz_offset = 3 * 8, // right stick Y
    .rz_size = 8,
    .rz_min = 0,
    .rz_max = 255,
    .rx_offset = 7 * 8, // L2 trigger
    .rx_size = 8,
    .rx_min = 0,
    .rx_max = 255,
    .ry_offset = 8 * 8, // R2 trigger
    .ry_size = 8,
    .ry_min = 0,
    .ry_max = 255,
    .hat_offset = 4 * 8, // D-pad
    .hat_size = 4,
    .hat_min = 0,
    .hat_max = 7,
    .button_offsets = {
        // X, Circle, Square, Triangle, L1, R1, Share, Options
        37, 38, 36, 39, 40, 41, 44, 45,
        // L2, R2, L3, R3, PS, Touchpad, Unused, Unused
        42, 43, 46, 47, 48, 49, 0xFFFF, 0xFFFF,
        // Hat buttons computed from HID hat
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};

static const des_gamepad_t __in_flash("hid_descriptors") des_sony_ds5 = {
    .valid = true,
    .sony = true,
    .report_id = 1,
    .x_offset = 0 * 8, // left stick X
    .x_size = 8,
    .x_min = 0,
    .x_max = 255,
    .y_offset = 1 * 8, // left stick Y
    .y_size = 8,
    .y_min = 0,
    .y_max = 255,
    .z_offset = 2 * 8, // right stick X
    .z_size = 8,
    .z_min = 0,
    .z_max = 255,
    .rz_offset = 3 * 8, // right stick Y
    .rz_size = 8,
    .rz_min = 0,
    .rz_max = 255,
    .rx_offset = 4 * 8, // L2 trigger
    .rx_size = 8,
    .rx_min = 0,
    .rx_max = 255,
    .ry_offset = 5 * 8, // R2 trigger
    .ry_size = 8,
    .ry_min = 0,
    .ry_max = 255,
    .hat_offset = 7 * 8, // D-pad
    .hat_size = 4,
    .hat_min = 0,
    .hat_max = 7,
    .button_offsets = {
        // X, Circle, Square, Triangle, L1, R1, Create, Options
        61, 62, 60, 63, 64, 65, 68, 69,
        // L2, R2, L3, R3, PS, Touchpad, Mute, Unused
        66, 67, 70, 71, 72, 73, 74, 0xFFFF,
        // Hat buttons computed from HID hat
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};

static bool des_is_sony_ds4(uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id == 0x054C) // Sony Interactive Entertainment
    {
        switch (product_id)
        {
        case 0x05C4: // DualShock 4 Controller (1st gen)
        case 0x09CC: // DualShock 4 Controller (2nd gen)
        case 0x0BA0: // DualShock 4 USB receiver
        case 0x0DAE: // DualShock 4 (special edition variant)
        case 0x0DF2: // DualShock 4 (special edition variant)
        case 0x0CDA: // DualShock 4 (Asia region, special edition)
        case 0x0D9A: // DualShock 4 (Japan region, special edition)
        case 0x0E04: // DualShock 4 (rare, but reported)
        case 0x0E6F: // DualShock 4 (special edition, sometimes used for DS4)
        case 0x0EBA: // DualShock 4 (special edition, sometimes used for DS4)
            return true;
        }
    }
    if (vendor_id == 0x0C12) // Zeroplus/Cirka
    {
        switch (product_id)
        {
        case 0x1E1A: // Cirka Wired Controller
        case 0x0E10: // Zeroplus PS4 compatible controller
        case 0x0E20: // Zeroplus PS4 compatible controller
            return true;
        }
    }
    if (vendor_id == 0x20D6) // PowerA
    {
        switch (product_id)
        {
        case 0xA711: // PowerA PS4 Wired Controller
            return true;
        }
    }
    if (vendor_id == 0x24C6) // PowerA (formerly BDA, LLC)
    {
        switch (product_id)
        {
        case 0x5501: // PowerA PS4 Wired Controller
            return true;
        }
    }
    if (vendor_id == 0x0F0D) // Hori
    {
        switch (product_id)
        {
        case 0x0055: // Hori PS4 Mini Wired Gamepad
        case 0x005E: // Hori PS4 Mini Wired Gamepad
        case 0x00C5: // Hori PS4 Fighting Commander
        case 0x00D9: // Hori PS4 Fighting Stick Mini
        case 0x00EE: // Hori PS4 Fighting Commander
        case 0x00F6: // Hori PS4 Mini Gamepad
        case 0x00F7: // Hori PS4 Mini Gamepad
            return true;
        }
    }
    return false;
}

static bool des_is_sony_ds5(uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id == 0x054C) // Sony Interactive Entertainment
    {
        switch (product_id)
        {
        case 0x0CE6: // DualSense Controller
        case 0x0DF2: // DualSense Edge Controller
        case 0x0E5C: // DualSense (special edition Spider-Man 2)
        case 0x0E8A: // DualSense (special edition FF16)
        case 0x0E9A: // DualSense (special edition LeBron James)
        case 0x0E6F: // DualSense (special edition Gray Camouflage)
        case 0x0E9C: // DualSense (special edition Volcanic Red)
        case 0x0EA6: // DualSense (special edition Sterling Silver)
        case 0x0EBA: // DualSense (special edition Cobalt Blue)
        case 0x0ED0: // DualSense (special edition Midnight Black V2)
            return true;
        }
    }
    if (vendor_id == 0x0F0D) // Hori (third-party DualSense compatible)
    {
        switch (product_id)
        {
        case 0x0184: // Hori DualSense compatible (Onyx Plus, etc)
        case 0x019C: // Hori Fighting Commander OCTA for PS5
        case 0x01A0: // Hori Fighting Stick α for PS5
            return true;
        }
    }
    return false;
}

static void des_parse_hid_controller(des_gamepad_t *gamepad, uint8_t const *desc_report, uint16_t desc_len)
{
    memset(gamepad, 0, sizeof(des_gamepad_t));
    for (int i = 0; i < PAD_MAX_BUTTONS; i++)
        gamepad->button_offsets[i] = 0xFFFF;

    // Use BTstack HID parser to parse the descriptor
    btstack_hid_usage_iterator_t iterator;
    btstack_hid_usage_iterator_init(&iterator, desc_report, desc_len, HID_REPORT_TYPE_INPUT);
    while (btstack_hid_usage_iterator_has_more(&iterator))
    {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&iterator, &item);

        // Store report ID if this is the first one we encounter
        if (gamepad->report_id == 0 && item.report_id != 0xFFFF)
            gamepad->report_id = item.report_id;

        // Map usages to gamepad fields
        if (item.usage_page == 0x01) // Generic Desktop
        {
            switch (item.usage)
            {
            case 0x30: // X axis (left stick X)
                gamepad->x_offset = item.bit_pos;
                gamepad->x_size = item.size;
                gamepad->x_min = iterator.global_logical_minimum;
                gamepad->x_max = iterator.global_logical_maximum;
                break;
            case 0x31: // Y axis (left stick Y)
                gamepad->y_offset = item.bit_pos;
                gamepad->y_size = item.size;
                gamepad->y_min = iterator.global_logical_minimum;
                gamepad->y_max = iterator.global_logical_maximum;
                break;
            case 0x32: // Z axis (right stick X)
                gamepad->z_offset = item.bit_pos;
                gamepad->z_size = item.size;
                gamepad->z_min = iterator.global_logical_minimum;
                gamepad->z_max = iterator.global_logical_maximum;
                break;
            case 0x35: // Rz axis (right stick Y)
                gamepad->rz_offset = item.bit_pos;
                gamepad->rz_size = item.size;
                gamepad->rz_min = iterator.global_logical_minimum;
                gamepad->rz_max = iterator.global_logical_maximum;
                break;
            case 0x33: // Rx axis (left trigger)
                gamepad->rx_offset = item.bit_pos;
                gamepad->rx_size = item.size;
                gamepad->rx_min = iterator.global_logical_minimum;
                gamepad->rx_max = iterator.global_logical_maximum;
                break;
            case 0x34: // Ry axis (right trigger)
                gamepad->ry_offset = item.bit_pos;
                gamepad->ry_size = item.size;
                gamepad->ry_min = iterator.global_logical_minimum;
                gamepad->ry_max = iterator.global_logical_maximum;
                break;
            case 0x39: // Hat switch (D-pad)
                gamepad->hat_offset = item.bit_pos;
                gamepad->hat_size = item.size;
                gamepad->hat_min = iterator.global_logical_minimum;
                gamepad->hat_max = iterator.global_logical_maximum;
                break;
            }
        }
        else if (item.usage_page == 0x09) // Button page
        {
            uint8_t button_index = item.usage - 1; // Buttons 1-indexed
            if (button_index < PAD_MAX_BUTTONS)
                gamepad->button_offsets[button_index] = item.bit_pos;
        }
    }

    // If it quacks like a joystick
    if (gamepad->x_size || gamepad->y_size || gamepad->z_size ||
        gamepad->rz_size || gamepad->rx_size || gamepad->ry_size ||
        gamepad->hat_size || gamepad->button_offsets[0] != 0xFFFF)
        gamepad->valid = true;
}

void des_report_descriptor(des_gamepad_t *gamepad,
                           uint8_t const *desc_report, uint16_t desc_len,
                           uint8_t dev_addr, uint16_t vendor_id, uint16_t product_id)
{
    gamepad->valid = false;
    des_parse_hid_controller(gamepad, desc_report, desc_len);

    DBG("Received HID descriptor. vid=0x%04X, pid=0x%04X, len=%d, valid=%d\n",
        vendor_id, product_id, desc_len, gamepad->valid);

    // Remap HID buttons for known vendors and products
    if (gamepad->valid)
    {
        des_remap_playstation_classic(gamepad, vendor_id);
        des_remap_8bitdo_dinput(gamepad, vendor_id, product_id);
        // add yours here
    }

    // Non HID controllers use a pre-computed descriptor
    if (desc_len == 0)
    {
        // Xbox controllers use XInput protocol
        if (xin_is_xbox_one(dev_addr))
        {
            *gamepad = des_xbox_one;
            DBG("Detected Xbox One controller, using pre-computed descriptor.\n");
        }

        // Xbox controllers use XInput protocol
        if (xin_is_xbox_360(dev_addr))
        {
            *gamepad = des_xbox_360;
            DBG("Detected Xbox 360 controller, using pre-computed descriptor.\n");
        }

        // Sony DualShock 4 controllers
        if (des_is_sony_ds4(vendor_id, product_id))
        {
            *gamepad = des_sony_ds4;
            DBG("Detected Sony DS4 controller, using pre-computed descriptor.\n");
        }

        // Sony DualShock 5 controllers
        if (des_is_sony_ds5(vendor_id, product_id))
        {
            *gamepad = des_sony_ds5;
            DBG("Detected Sony DS5 controller, using pre-computed descriptor.\n");
        }
    }

    if (!gamepad->valid)
        DBG("HID descriptor not a gamepad.\n");
}
