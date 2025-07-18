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

#define DEBUG_RIA_USB_DES

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_DES)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static const des_gamepad_t __in_flash("hid_descriptors") des_xbox_one = {
    .valid = true,
    .report_id = 0x20, // GIP message ID
    .x_offset = 9 * 8, // left stick X
    .x_size = 16,
    .x_logical_min = -32768,
    .x_logical_max = 32767,
    .y_offset = 11 * 8, // left stick Y
    .y_size = 16,
    .y_logical_min = 32767,
    .y_logical_max = -32768,
    .z_offset = 13 * 8, // right stick X
    .z_size = 16,
    .z_logical_min = -32768,
    .z_logical_max = 32767,
    .rz_offset = 15 * 8, // right stick Y
    .rz_size = 16,
    .rz_logical_min = 32767,
    .rz_logical_max = -32768,
    .rx_offset = 5 * 8, // left trigger
    .rx_size = 10,
    .rx_logical_min = 0,
    .rx_logical_max = 1023,
    .ry_offset = 7 * 8, // right trigger
    .ry_size = 10,
    .ry_logical_min = 0,
    .ry_logical_max = 1023,
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
        0xFFFF,    // Xbox guide button (sent via separate GIP_CMD_VIRTUAL_KEY report)
        0xFFFF,    // unused
        0xFFFF,    // unused
        0xFFFF,    // unused
        //
        4 * 8 + 0, // D-pad Up
        4 * 8 + 1, // D-pad Down
        4 * 8 + 2, // D-pad Left
        4 * 8 + 3, // D-pad Right
    }};

// Xbox 360 controllers use a different report structure than Xbox One:
// - No report ID for input reports
// - 16-bit signed analog stick values
// - 8-bit trigger values (0-255)
// - D-pad as individual button bits (not hat switch)
// - Different button layout and offsets
static const des_gamepad_t __in_flash("hid_descriptors") des_xbox_360 = {
    .valid = true,
    .report_id = 0,    // Xbox 360 uses no report ID for input reports
    .x_offset = 6 * 8, // left stick X
    .x_size = 16,
    .x_logical_min = -32768,
    .x_logical_max = 32767,
    .y_offset = 8 * 8, // left stick Y
    .y_size = 16,
    .y_logical_min = 32767,
    .y_logical_max = -32768,
    .z_offset = 10 * 8, // right stick X
    .z_size = 16,
    .z_logical_min = -32768,
    .z_logical_max = 32767,
    .rz_offset = 12 * 8, // right stick Y
    .rz_size = 16,
    .rz_logical_min = 32767,
    .rz_logical_max = -32768,
    .rx_offset = 4 * 8, // left trigger
    .rx_size = 8,
    .rx_logical_min = 0,
    .rx_logical_max = 255,
    .ry_offset = 5 * 8, // right trigger
    .ry_size = 8,
    .ry_logical_min = 0,
    .ry_logical_max = 255,
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
        3 * 8 + 2, // Guide button
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
    .x_logical_min = 0,
    .x_logical_max = 255,
    .y_offset = 1 * 8, // left stick Y
    .y_size = 8,
    .y_logical_min = 0,
    .y_logical_max = 255,
    .z_offset = 2 * 8, // right stick X
    .z_size = 8,
    .z_logical_min = 0,
    .z_logical_max = 255,
    .rz_offset = 3 * 8, // right stick Y
    .rz_size = 8,
    .rz_logical_min = 0,
    .rz_logical_max = 255,
    .rx_offset = 7 * 8, // L2 trigger
    .rx_size = 8,
    .rx_logical_min = 0,
    .rx_logical_max = 255,
    .ry_offset = 8 * 8, // R2 trigger
    .ry_size = 8,
    .ry_logical_min = 0,
    .ry_logical_max = 255,
    .hat_offset = 4 * 8, // D-pad
    .hat_size = 4,
    .hat_logical_min = 0,
    .hat_logical_max = 7,
    .button_offsets = {
        // X, Circle, Square, Triangle, L1, R1, Share, Options
        37, 38, 36, 39, 40, 41, 44, 45,
        // L2, R2, L3, R3, PS, Touchpad, Unused, Unused
        42, 43, 46, 47, 48, 49, 0xFFFF, 0xFFFF,
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
        case 0x0E20: // Zeroplus PS4 compatible controller (alternate)
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
        case 0x0055: // Hori PS4 Mini Wired Gamepad (alternate)
        case 0x005E: // Hori PS4 Mini Wired Gamepad
        case 0x00C5: // Hori PS4 Fighting Commander (alternate)
        case 0x00D9: // Hori PS4 Fighting Stick Mini
        case 0x00EE: // Hori PS4 Fighting Commander
        case 0x00F6: // Hori PS4 Mini Gamepad (alternate)
        case 0x00F7: // Hori PS4 Mini Gamepad (alternate)
            return true;
        }
    }
    return false;
}

// TODO this is untested as I don't have a DS5 gamepad
static const des_gamepad_t __in_flash("hid_descriptors") des_sony_ds5 = {
    .valid = true,
    .sony = true,
    .report_id = 1,
    .x_offset = 0 * 8, // left stick X
    .x_size = 8,
    .x_logical_min = 0,
    .x_logical_max = 255,
    .y_offset = 1 * 8, // left stick Y
    .y_size = 8,
    .y_logical_min = 0,
    .y_logical_max = 255,
    .z_offset = 2 * 8, // right stick X
    .z_size = 8,
    .z_logical_min = 0,
    .z_logical_max = 255,
    .rz_offset = 3 * 8, // right stick Y
    .rz_size = 8,
    .rz_logical_min = 0,
    .rz_logical_max = 255,
    .rx_offset = 4 * 8, // L2 trigger
    .rx_size = 8,
    .rx_logical_min = 0,
    .rx_logical_max = 255,
    .ry_offset = 5 * 8, // R2 trigger
    .ry_size = 8,
    .ry_logical_min = 0,
    .ry_logical_max = 255,
    .hat_offset = 7 * 8, // D-pad
    .hat_size = 4,
    .hat_logical_min = 0,
    .hat_logical_max = 7,
    .button_offsets = {
        // X, Circle, Square, Triangle, L1, R1, Create, Options
        61, 62, 60, 63, 64, 65, 68, 69,
        // L2, R2, L3, R3, PS, Touchpad, Unused, Unused
        66, 67, 70, 71, 72, 73, 0xFFFF, 0xFFF,
        // Hat buttons computed from HID hat
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};

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

static void des_remap_8bitdo_dinput(des_gamepad_t *desc, uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id != 0x2DC8) // 8BitDo
        return;
    DBG("Remapping 8BitDo Dinput buttons.\n");
    // All 8BitDo controllers in DInput mode have "gaps" in their buttons.
    uint16_t temp2 = desc->button_offsets[2];
    uint16_t temp5 = desc->button_offsets[5];
    desc->button_offsets[2] = desc->button_offsets[3];
    desc->button_offsets[3] = desc->button_offsets[4];
    for (int i = 4; i <= 9; i++)
        desc->button_offsets[i] = desc->button_offsets[i + 2];
    desc->button_offsets[10] = desc->button_offsets[13];
    desc->button_offsets[11] = desc->button_offsets[14];
    // Swap buttons 6,7 with 8,9
    uint16_t temp6 = desc->button_offsets[6];
    uint16_t temp7 = desc->button_offsets[7];
    desc->button_offsets[6] = desc->button_offsets[8];
    desc->button_offsets[7] = desc->button_offsets[9];
    desc->button_offsets[8] = temp6;
    desc->button_offsets[9] = temp7;
    // M30 wired special case
    if (product_id == 0x5006)
    {
        // unsual mapping for the guide button only when wired.
        uint16_t temp12 = desc->button_offsets[12];
        desc->button_offsets[12] = temp2;
        temp2 = temp12;
    }
    // Drop the gaps at the end, not sure what uses this.
    desc->button_offsets[13] = temp2;
    desc->button_offsets[14] = temp5;
}

static void des_parse_hid_controller(des_gamepad_t *desc, uint8_t const *desc_report, uint16_t desc_len)
{
    memset(desc, 0, sizeof(des_gamepad_t));
    desc->hid = true;
    for (int i = 0; i < PAD_MAX_BUTTONS; i++)
        desc->button_offsets[i] = 0xFFFF;

    // Use BTstack HID parser to parse the descriptor
    btstack_hid_usage_iterator_t usage_iterator;
    btstack_hid_usage_iterator_init(&usage_iterator, desc_report, desc_len, HID_REPORT_TYPE_INPUT);

    // Iterate through all input usages to find gamepad controls
    while (btstack_hid_usage_iterator_has_more(&usage_iterator))
    {
        btstack_hid_usage_item_t usage_item;
        btstack_hid_usage_iterator_get_item(&usage_iterator, &usage_item);

        // Store report ID if this is the first one we encounter
        if (desc->report_id == 0 && usage_item.report_id != 0xFFFF)
            desc->report_id = usage_item.report_id;

        // Map usages to gamepad fields
        if (usage_item.usage_page == 0x01) // Generic Desktop
        {
            switch (usage_item.usage)
            {
            case 0x30: // X axis (left stick X)
                desc->x_offset = usage_item.bit_pos;
                desc->x_size = usage_item.size;
                desc->x_logical_min = usage_iterator.global_logical_minimum;
                desc->x_logical_max = usage_iterator.global_logical_maximum;
                break;
            case 0x31: // Y axis (left stick Y)
                desc->y_offset = usage_item.bit_pos;
                desc->y_size = usage_item.size;
                desc->y_logical_min = usage_iterator.global_logical_minimum;
                desc->y_logical_max = usage_iterator.global_logical_maximum;
                break;
            case 0x32: // Z axis (right stick X)
                desc->z_offset = usage_item.bit_pos;
                desc->z_size = usage_item.size;
                desc->z_logical_min = usage_iterator.global_logical_minimum;
                desc->z_logical_max = usage_iterator.global_logical_maximum;
                break;
            case 0x35: // Rz axis (right stick Y)
                desc->rz_offset = usage_item.bit_pos;
                desc->rz_size = usage_item.size;
                desc->rz_logical_min = usage_iterator.global_logical_minimum;
                desc->rz_logical_max = usage_iterator.global_logical_maximum;
                break;
            case 0x33: // Rx axis (left trigger)
                desc->rx_offset = usage_item.bit_pos;
                desc->rx_size = usage_item.size;
                desc->rx_logical_min = usage_iterator.global_logical_minimum;
                desc->rx_logical_max = usage_iterator.global_logical_maximum;
                break;
            case 0x34: // Ry axis (right trigger)
                desc->ry_offset = usage_item.bit_pos;
                desc->ry_size = usage_item.size;
                desc->ry_logical_min = usage_iterator.global_logical_minimum;
                desc->ry_logical_max = usage_iterator.global_logical_maximum;
                break;
            case 0x39: // Hat switch (D-pad)
                desc->hat_offset = usage_item.bit_pos;
                desc->hat_size = usage_item.size;
                desc->hat_logical_min = usage_iterator.global_logical_minimum;
                desc->hat_logical_max = usage_iterator.global_logical_maximum;
                break;
            }
        }
        else if (usage_item.usage_page == 0x09) // Button page
        {
            uint8_t button_index = usage_item.usage - 1; // Buttons 1-indexed
            if (button_index < PAD_MAX_BUTTONS)
                desc->button_offsets[button_index] = usage_item.bit_pos;
        }
    }

    // If it quacks like a joystick
    if (desc->x_size || desc->y_size || desc->z_size ||
        desc->rz_size || desc->rx_size || desc->ry_size ||
        desc->hat_size || desc->button_offsets[0] != 0xFFFF)
        desc->valid = true;
}

void des_report_descriptor(des_gamepad_t *desc,
                           uint8_t const *desc_report, uint16_t desc_len,
                           uint8_t dev_addr, uint16_t vendor_id, uint16_t product_id)
{
    DBG("Received HID descriptor. vid=0x%04X, pid=0x%04X, len=%d\n", vendor_id, product_id, desc_len);
    desc->valid = false;

    des_parse_hid_controller(desc, desc_report, desc_len);
    DBG("Parsed valid=%d hid=%d\n", desc->valid, desc->hid);

    // Only HID gamepads may pass. Except...
    // Xbox and Sony don't always have a descriptor.
    if (desc_len && !desc->valid)
        return;

    // Xbox controllers use XInput protocol
    if (xin_is_xbox_one(dev_addr))
    {
        *desc = des_xbox_one;
        DBG("Detected Xbox One controller, using pre-computed descriptor.\n");
    }

    // Xbox controllers use XInput protocol
    if (xin_is_xbox_360(dev_addr))
    {
        *desc = des_xbox_360;
        DBG("Detected Xbox 360 controller, using pre-computed descriptor.\n");
    }

    // Sony DualShock 4 controllers don't have HID descriptor
    if (des_is_sony_ds4(vendor_id, product_id))
    {
        *desc = des_sony_ds4;
        DBG("Detected Sony DS4 controller, using pre-computed descriptor.\n");
    }

    // Sony DualShock 5 controllers don't have HID descriptor
    if (des_is_sony_ds5(vendor_id, product_id))
    {
        *desc = des_sony_ds5;
        DBG("Detected Sony DS5 controller, using pre-computed descriptor.\n");
    }

    if (desc->valid && desc->hid)
    {
        // Remap HID buttons for known vendors and products
        des_remap_8bitdo_dinput(desc, vendor_id, product_id);
        // add yours here
    }

    if (!desc->valid)
        DBG("HID descriptor not a gamepad.\n");
    else
    {
        DBG("HID descriptor parsing result:\n");
        DBG("  Report ID: %d\n", desc->report_id);
        DBG("  X: offset=%d, size=%d\n", desc->x_offset, desc->x_size);
        DBG("  Y: offset=%d, size=%d\n", desc->y_offset, desc->y_size);
        DBG("  Z: offset=%d, size=%d\n", desc->z_offset, desc->z_size);
        DBG("  Rz: offset=%d, size=%d\n", desc->rz_offset, desc->rz_size);
        DBG("  Rx: offset=%d, size=%d\n", desc->rx_offset, desc->rx_size);
        DBG("  Ry: offset=%d, size=%d\n", desc->ry_offset, desc->ry_size);
        DBG("  Hat: offset=%d, size=%d\n", desc->hat_offset, desc->hat_size);
        DBG("  Button offsets: ");
        for (int i = 0; i < PAD_MAX_BUTTONS && desc->button_offsets[i] != 0xFFFF; i++)
            DBG("%d ", (int16_t)desc->button_offsets[i]);
        DBG("\n");
    }
}
