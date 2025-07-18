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
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static const des_gamepad_t __in_flash("hid_descriptors") xbox_one_descriptor = {
    .valid = true,
    .sony = false,
    .report_id = 0x20, // GIP message ID
    .x_offset = 9 * 8, // Byte 9-10 (left stick X) - 16-bit signed little endian
    .x_size = 16,
    .x_logical_min = -32768, // 16-bit signed range
    .x_logical_max = 32767,
    .y_offset = 11 * 8, // Byte 11-12 (left stick Y) - 16-bit signed little endian
    .y_size = 16,
    .y_logical_min = 32767, // 16-bit signed range
    .y_logical_max = -32768,
    .z_offset = 13 * 8, // Byte 13-14 (right stick X) - 16-bit signed little endian
    .z_size = 16,
    .z_logical_min = -32768, // 16-bit signed range
    .z_logical_max = 32767,
    .rz_offset = 15 * 8, // Byte 15-16 (right stick Y) - 16-bit signed little endian
    .rz_size = 16,
    .rz_logical_min = 32767, // 16-bit signed range
    .rz_logical_max = -32768,
    .rx_offset = 5 * 8, // Byte 5-6 (left trigger) - 16-bit unsigned little endian
    .rx_size = 10,
    .rx_logical_min = 0, // Triggers are unsigned 0-65535 (full 16-bit range)
    .rx_logical_max = 1023,
    .ry_offset = 7 * 8, // Byte 7-8 (right trigger) - 16-bit unsigned little endian
    .ry_size = 10,
    .ry_logical_min = 0, // Triggers are unsigned 0-65535 (full 16-bit range)
    .ry_logical_max = 1023,
    .hat_offset = 0, // Xbox One uses individual dpad buttons, not hat switch
    .hat_size = 0,
    .hat_logical_min = 0,
    .hat_logical_max = 0,

    // DS4 button layout: X, Circle, Square, Triangle, L1, R1, L2, R2,
    // Share, Options, L3, R3, PS, Touchpad

    .button_offsets = {
        // Xbox One Gamepad Input Protocol button layout (GIP_CMD_INPUT reports)
        3 * 8 + 4, // A button (bit 4 of byte 3)
        3 * 8 + 5, // B button (bit 5 of byte 3)
        3 * 8 + 6, // X button (bit 6 of byte 3)
        3 * 8 + 7, // Y button (bit 7 of byte 3)
        4 * 8 + 4, // Left shoulder/LB (bit 4 of byte 4)
        4 * 8 + 5, // Right shoulder/RB (bit 5 of byte 4)
        3 * 8 + 3, // View/Select button (bit 3 of byte 3)
        3 * 8 + 2, // Menu/Start button (bit 2 of byte 3)
        //
        0xFFFF,    // L2 (analog trigger)
        0xFFFF,    // R2 (analog trigger)
        4 * 8 + 6, // Left stick click (bit 1 of byte 4)
        4 * 8 + 7, // Right stick click (bit 0 of byte 4)
        0xFFFF,    // Xbox guide button (sent via separate GIP_CMD_VIRTUAL_KEY report)
        0xFFFF,    // unused
        0xFFFF,    // unused
        0xFFFF,    // unused
        //
        4 * 8 + 0, // D-pad Up (bit 0 of byte 4)
        4 * 8 + 1, // D-pad Down (bit 1 of byte 4)
        4 * 8 + 2, // D-pad Left (bit 2 of byte 4)
        4 * 8 + 3, // D-pad Right (bit 3 of byte 4)
    }};

// Xbox 360 controllers use a different report structure than Xbox One:
// - No report ID for input reports
// - 16-bit signed analog stick values
// - 8-bit trigger values (0-255)
// - D-pad as individual button bits (not hat switch)
// - Different button layout and offsets
static const des_gamepad_t __in_flash("hid_descriptors") xbox_360_descriptor = {
    .valid = true,
    .sony = false,
    .report_id = 0,    // Xbox 360 uses no report ID for input reports
    .x_offset = 6 * 8, // Byte 6 (left stick X) - 16-bit signed
    .x_size = 16,
    .x_logical_min = -32768, // 16-bit signed range
    .x_logical_max = 32767,
    .y_offset = 8 * 8, // Byte 8 (left stick Y) - 16-bit signed
    .y_size = 16,
    .y_logical_min = -32768, // 16-bit signed range
    .y_logical_max = 32767,
    .z_offset = 10 * 8, // Byte 10 (right stick X) - 16-bit signed
    .z_size = 16,
    .z_logical_min = -32768, // 16-bit signed range
    .z_logical_max = 32767,
    .rz_offset = 12 * 8, // Byte 12 (right stick Y) - 16-bit signed
    .rz_size = 16,
    .rz_logical_min = -32768, // 16-bit signed range
    .rz_logical_max = 32767,
    .rx_offset = 4 * 8, // Byte 4 (left trigger) - 8-bit unsigned
    .rx_size = 8,
    .rx_logical_min = 0, // Triggers are unsigned 0-255 (8-bit)
    .rx_logical_max = 255,
    .ry_offset = 5 * 8, // Byte 5 (right trigger) - 8-bit unsigned
    .ry_size = 8,
    .ry_logical_min = 0, // Triggers are unsigned 0-255 (8-bit)
    .ry_logical_max = 255,
    .hat_offset = 0, // Xbox 360 uses individual dpad buttons, not hat switch
    .hat_size = 0,
    .hat_logical_min = 0,
    .hat_logical_max = 0,
    .button_offsets = {
        // Xbox 360 button layout based on XInput standard (matches Linux xpad driver)
        // Face buttons are in byte 3, bits 4-7
        3 * 8 + 4, // A button (bit 4 of byte 3)
        3 * 8 + 5, // B button (bit 5 of byte 3)
        3 * 8 + 6, // X button (bit 6 of byte 3)
        3 * 8 + 7, // Y button (bit 7 of byte 3)
        3 * 8 + 0, // Left shoulder/LB (bit 0 of byte 3)
        3 * 8 + 1, // Right shoulder/RB (bit 1 of byte 3)
        2 * 8 + 5, // Back/Select button (bit 5 of byte 2)
        2 * 8 + 4, // Start button (bit 4 of byte 2)
        //
        0xFFFF,    // L2 (analog trigger)
        0xFFFF,    // R2 (analog trigger)
        2 * 8 + 6, // Left stick click (bit 6 of byte 2)
        2 * 8 + 7, // Right stick click (bit 7 of byte 2)
        3 * 8 + 2, // Xbox guide button (bit 2 of byte 3)
        0xFFFF,    // unused
        0xFFFF,    // unused
        0xFFFF,    // unused
        //
        2 * 8 + 0, // D-pad Up (bit 0 of byte 2)
        2 * 8 + 1, // D-pad Down (bit 1 of byte 2)
        2 * 8 + 2, // D-pad Left (bit 2 of byte 2)
        2 * 8 + 3  // D-pad Right (bit 3 of byte 2)
    }};

static const des_gamepad_t __in_flash("hid_descriptors") ds4_descriptor = {
    .valid = true,
    .sony = true,
    .report_id = 1,
    .x_offset = 0 * 8, // Byte 0 (left stick X) - after report ID is stripped
    .x_size = 8,
    .x_logical_min = 0, // DS4 uses 8-bit unsigned 0-255
    .x_logical_max = 255,
    .y_offset = 1 * 8, // Byte 1 (left stick Y)
    .y_size = 8,
    .y_logical_min = 0, // DS4 uses 8-bit unsigned 0-255
    .y_logical_max = 255,
    .z_offset = 2 * 8, // Byte 2 (right stick X)
    .z_size = 8,
    .z_logical_min = 0, // DS4 uses 8-bit unsigned 0-255
    .z_logical_max = 255,
    .rz_offset = 3 * 8, // Byte 3 (right stick Y)
    .rz_size = 8,
    .rz_logical_min = 0, // DS4 uses 8-bit unsigned 0-255
    .rz_logical_max = 255,
    .rx_offset = 7 * 8, // Byte 7 (L2 trigger) - after report ID is stripped
    .rx_size = 8,
    .rx_logical_min = 0, // DS4 triggers are 8-bit unsigned 0-255
    .rx_logical_max = 255,
    .ry_offset = 8 * 8, // Byte 8 (R2 trigger)
    .ry_size = 8,
    .ry_logical_min = 0, // DS4 triggers are 8-bit unsigned 0-255
    .ry_logical_max = 255,
    .hat_offset = 4 * 8, // Byte 4, lower nibble (D-pad)
    .hat_size = 4,
    .hat_logical_min = 0, // Hat values 0-7, 8=none
    .hat_logical_max = 8,
    .button_offsets = {
        // X, Circle, Square, Triangle, L1, R1, Share, Options
        37, 38, 36, 39, 40, 41, 44, 45,
        // L2, R2, L3, R3, PS, Touchpad, Unused, Unused
        42, 43, 46, 47, 48, 49, 0xFFFF, 0xFFFF,
        // Hat buttons computed from HID hat
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};

static void des_sony_ds4_controller(des_gamepad_t *desc, uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id == 0x054C) // Sony Interactive Entertainment
    {
        switch (product_id)
        {
        case 0x05C4: // DualShock 4 Controller
        case 0x09CC: // DualShock 4 Controller (2nd generation)
        case 0x0BA0: // DualShock 4 USB receiver
            *desc = ds4_descriptor;
        }
    }
    if (vendor_id == 0x0C12) // Zeroplus/Cirka
    {
        switch (product_id)
        {
        case 0x1E1A: // Cirka Wired Controller
            *desc = ds4_descriptor;
        }
    }
}

static const des_gamepad_t __in_flash("hid_descriptors") ds5_descriptor = {
    .valid = true,
    .sony = true,
    .report_id = 1,
    .x_offset = 0 * 8, // Byte 0 (left stick X) - after report ID is stripped
    .x_size = 8,
    .x_logical_min = 0, // DS5 uses 8-bit unsigned 0-255
    .x_logical_max = 255,
    .y_offset = 1 * 8, // Byte 1 (left stick Y)
    .y_size = 8,
    .y_logical_min = 0, // DS5 uses 8-bit unsigned 0-255
    .y_logical_max = 255,
    .z_offset = 2 * 8, // Byte 2 (right stick X)
    .z_size = 8,
    .z_logical_min = 0, // DS5 uses 8-bit unsigned 0-255
    .z_logical_max = 255,
    .rz_offset = 3 * 8, // Byte 3 (right stick Y)
    .rz_size = 8,
    .rz_logical_min = 0, // DS5 uses 8-bit unsigned 0-255
    .rz_logical_max = 255,
    .rx_offset = 4 * 8, // Byte 4 (L2 trigger) - after report ID is stripped
    .rx_size = 8,
    .rx_logical_min = 0, // DS5 triggers are 8-bit unsigned 0-255
    .rx_logical_max = 255,
    .ry_offset = 5 * 8, // Byte 5 (R2 trigger)
    .ry_size = 8,
    .ry_logical_min = 0, // DS5 triggers are 8-bit unsigned 0-255
    .ry_logical_max = 255,
    .hat_offset = 7 * 8, // Byte 7, lower nibble (D-pad)
    .hat_size = 4,
    .hat_logical_min = 0, // Hat values 0-7, 8=none
    .hat_logical_max = 8,
    .button_offsets = {
        // X, Circle, Square, Triangle, L1, R1, Create, Options
        61, 62, 60, 63, 64, 65, 68, 69,
        // L2, R2, L3, R3, PS, Touchpad, Unused, Unused
        66, 67, 70, 71, 72, 73, 0xFFFF, 0xFFF,
        // Hat buttons computed from HID hat
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};

static void des_sony_ds5_controller(des_gamepad_t *desc, uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id == 0x054C) // Sony Interactive Entertainment
    {
        switch (product_id)
        {
        case 0x0CE6: // DualSense Controller
        case 0x0DF2: // DualSense Edge Controller
            *desc = ds5_descriptor;
        }
    }
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

static void des_parse_generic_controller(des_gamepad_t *desc, uint8_t const *desc_report, uint16_t desc_len)
{
    memset(desc, 0, sizeof(des_gamepad_t));
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

    // Xbox controllers use XInput protocol
    if (!desc->valid)
    {
        if (xin_is_xbox_one(dev_addr))
        {
            *desc = xbox_one_descriptor;
            DBG("Detected Xbox One controller, using pre-computed descriptor.\n");
        }
    }

    // Xbox controllers use XInput protocol
    if (!desc->valid)
    {
        if (xin_is_xbox_360(dev_addr))
        {
            *desc = xbox_360_descriptor;
            DBG("Detected Xbox 360 controller, using pre-computed descriptor.\n");
        }
    }

    // Sony DualShock 4 controllers don't have a descriptor
    if (!desc->valid)
    {
        des_sony_ds4_controller(desc, vendor_id, product_id);
        if (desc->valid)
            DBG("Detected Sony DS4 controller, using pre-computed descriptor.\n");
    }

    // Sony DualShock 5 controllers don't have a descriptor
    if (!desc->valid)
    {
        des_sony_ds5_controller(desc, vendor_id, product_id);
        if (desc->valid)
            DBG("Detected Sony DS5 controller, using pre-computed descriptor.\n");
    }

    // Parse the HID descriptor for most controllers
    if (!desc->valid)
    {
        des_parse_generic_controller(desc, desc_report, desc_len);
        if (desc->valid)
            DBG("Detected generic controller, using parsed descriptor.\n");
    }

    // Remap the buttons for known vendors and products
    des_remap_8bitdo_dinput(desc, vendor_id, product_id);

    if (!desc->valid)
        DBG("HID descriptor not a gamepad.\n")
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
