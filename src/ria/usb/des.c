/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"
#include "des.h"
#include "btstack.h"
#include <string.h>

// #define DEBUG_RIA_USB_DES

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_DES)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...) \
    {            \
    }
#endif

// Xbox One XInput controller descriptors
// Based on Xbox One GameInput Protocol (GIP) as implemented in GP2040-CE
// Xbox One controllers use a specific report structure:
// - No report ID for input reports
// - 16-bit signed analog stick values
// - 16-bit trigger values (10-bit actual precision)
// - D-pad as individual button bits (not hat switch)
// - GIP header followed by button/analog data
static const pad_descriptor_t __in_flash("hid_descriptors") xbox_one_descriptor = {
    .valid = true,
    .sony = false,
    .report_id = 0,    // Xbox One uses no report ID for input reports
    .x_offset = 6 * 8, // Byte 6 (left stick X) - 16-bit signed
    .x_size = 16,
    .y_offset = 8 * 8, // Byte 8 (left stick Y) - 16-bit signed
    .y_size = 16,
    .z_offset = 10 * 8, // Byte 10 (right stick X) - 16-bit signed
    .z_size = 16,
    .rz_offset = 12 * 8, // Byte 12 (right stick Y) - 16-bit signed
    .rz_size = 16,
    .rx_offset = 4 * 8, // Byte 4 (left trigger) - 16-bit (10-bit actual)
    .rx_size = 16,
    .ry_offset = 4 * 8 + 16, // Byte 6 (right trigger) - 16-bit (10-bit actual)
    .ry_size = 16,
    .hat_offset = 0, // Xbox One uses individual dpad buttons, not hat switch
    .hat_size = 0,
    .button_offsets = {
        // Xbox One GIP report button layout based on GP2040-CE XBOneDescriptors.h
        // Byte 2 contains A, B, X, Y buttons
        2 * 8 + 0, // A button (byte 2, bit 0) - B1 mapping
        2 * 8 + 1, // B button (byte 2, bit 1) - B2 mapping
        2 * 8 + 2, // X button (byte 2, bit 2) - B3 mapping
        2 * 8 + 3, // Y button (byte 2, bit 3) - B4 mapping
        2 * 8 + 4, // Left shoulder (byte 2, bit 4) - L1 mapping
        2 * 8 + 5, // Right shoulder (byte 2, bit 5) - R1 mapping
        // L2/R2 are analog triggers, mapped through rx/ry
        0xFFFF,    // L2 (analog trigger)
        0xFFFF,    // R2 (analog trigger)
        1 * 8 + 2, // View/Back button (byte 1, bit 2) - S1 mapping
        1 * 8 + 3, // Menu/Start button (byte 1, bit 3) - S2 mapping
        2 * 8 + 6, // Left stick click (byte 2, bit 6) - L3 mapping
        2 * 8 + 7, // Right stick click (byte 2, bit 7) - R3 mapping
        // Guide button is handled separately via virtual keycode
        0xFFFF, // Guide (A1) - handled separately
        0xFFFF, // A2 - unused
        0xFFFF, // Extra buttons unused
        0xFFFF}};

static void des_xbox_one_controller(pad_descriptor_t *desc, uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id == 0x045E) // Microsoft
    {
        switch (product_id)
        {
        case 0x02D1: // Xbox One Controller
        case 0x02DD: // Xbox One Controller (Firmware 2015)
        case 0x02E3: // Xbox One Elite Controller
        case 0x02EA: // Xbox One S Controller
        case 0x02FD: // Xbox One S Controller (Bluetooth)
        case 0x0B00: // Xbox One Elite Controller Series 2
        case 0x0B05: // Xbox One Elite Controller Series 2 (Bluetooth)
        case 0x0B12: // Xbox Series X/S Controller
        case 0x0B13: // Xbox Series X/S Controller (Bluetooth)
        case 0x0B20: // Xbox Series X/S Controller (new firmware)
        case 0x0B21: // Xbox Series X/S Controller (newer firmware)
            *desc = xbox_one_descriptor;
            DBG("Detected Xbox One controller (PID: 0x%04X)\n", product_id);
            break;
        }
    }
    // Third-party Xbox One compatible controllers
    if (vendor_id == 0x0E6F) // PDP (Performance Designed Products)
    {
        switch (product_id)
        {
        case 0x0139: // PDP Xbox One Afterglow
        case 0x013A: // PDP Xbox One Rock Candy
        case 0x0146: // PDP Xbox One Rock Candy
        case 0x0147: // PDP Xbox One Afterglow
        case 0x015C: // PDP Xbox One Afterglow
        case 0x015D: // PDP Xbox One Rock Candy
        case 0x02A4: // PDP Xbox One Wired Controller
        case 0x02A6: // PDP Xbox One Controller
            *desc = xbox_one_descriptor;
            DBG("Detected PDP Xbox One controller (PID: 0x%04X)\n", product_id);
            break;
        }
    }
    if (vendor_id == 0x24C6) // PowerA
    {
        switch (product_id)
        {
        case 0x541A: // PowerA Xbox One Mini Pro Ex
        case 0x542A: // PowerA Xbox One Pro Ex
        case 0x543A: // PowerA Xbox One Mini Pro Ex
        case 0x561A: // PowerA FUSION Pro Controller
        case 0x581A: // PowerA FUSION Pro Controller
        case 0x591A: // PowerA FUSION Pro Controller
        case 0x791A: // PowerA FUSION Pro Controller for Xbox Series X|S
            *desc = xbox_one_descriptor;
            DBG("Detected PowerA Xbox One controller (PID: 0x%04X)\n", product_id);
            break;
        }
    }
    if (vendor_id == 0x0F0D) // Hori
    {
        switch (product_id)
        {
        case 0x0067: // Horipad for Xbox One
        case 0x0078: // Hori Real Arcade Pro V Kai Xbox One
        case 0x00C5: // Hori Fighting Commander for Xbox One
            *desc = xbox_one_descriptor;
            DBG("Detected Hori Xbox One controller (PID: 0x%04X)\n", product_id);
            break;
        }
    }
    if (vendor_id == 0x1532) // Razer
    {
        switch (product_id)
        {
        case 0x0A03: // Razer Wildcat for Xbox One
        case 0x0A14: // Razer Wolverine Ultimate for Xbox One
        case 0x0A15: // Razer Wolverine Tournament Edition
            *desc = xbox_one_descriptor;
            DBG("Detected Razer Xbox One controller (PID: 0x%04X)\n", product_id);
            break;
        }
    }
}

static const pad_descriptor_t __in_flash("hid_descriptors") ds4_descriptor = {
    .valid = true,
    .sony = true,
    .report_id = 1,
    .x_offset = 0 * 8, // Byte 0 (left stick X) - after report ID is stripped
    .x_size = 8,
    .y_offset = 1 * 8, // Byte 1 (left stick Y)
    .y_size = 8,
    .z_offset = 2 * 8, // Byte 2 (right stick X)
    .z_size = 8,
    .rz_offset = 3 * 8, // Byte 3 (right stick Y)
    .rz_size = 8,
    .rx_offset = 7 * 8, // Byte 7 (L2 trigger) - after report ID is stripped
    .rx_size = 8,
    .ry_offset = 8 * 8, // Byte 8 (R2 trigger)
    .ry_size = 8,
    .hat_offset = 4 * 8, // Byte 4, lower nibble (D-pad)
    .hat_size = 4,
    .button_offsets = {
        // DS4 button layout: X, Circle, Square, Triangle, L1, R1, L2, R2, Share, Options, L3, R3, PS, Touchpad
        37, 38, 36, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
        // Mark unused buttons with 0xFFFF
        0xFFFF, 0xFFFF}};

static void des_sony_ds4_controller(pad_descriptor_t *desc, uint16_t vendor_id, uint16_t product_id)
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

static const pad_descriptor_t __in_flash("hid_descriptors") ds5_descriptor = {
    .valid = true,
    .sony = true,
    .report_id = 1,
    .x_offset = 0 * 8, // Byte 0 (left stick X) - after report ID is stripped
    .x_size = 8,
    .y_offset = 1 * 8, // Byte 1 (left stick Y)
    .y_size = 8,
    .z_offset = 2 * 8, // Byte 2 (right stick X)
    .z_size = 8,
    .rz_offset = 3 * 8, // Byte 3 (right stick Y)
    .rz_size = 8,
    .rx_offset = 4 * 8, // Byte 4 (L2 trigger) - after report ID is stripped
    .rx_size = 8,
    .ry_offset = 5 * 8, // Byte 5 (R2 trigger)
    .ry_size = 8,
    .hat_offset = 7 * 8, // Byte 7, lower nibble (D-pad)
    .hat_size = 4,
    .button_offsets = {
        // DS5 button layout: X, Circle, Square, Triangle, L1, R1, L2, R2, Create, Options, L3, R3, PS, Touchpad
        61, 62, 60, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73,
        // Mark unused buttons with 0xFFFF
        0xFFFF, 0xFFFF}};

static void des_sony_ds5_controller(pad_descriptor_t *desc, uint16_t vendor_id, uint16_t product_id)
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

static void des_remap_8bitdo_dinput(pad_descriptor_t *desc, uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id != 0x2DC8) // 8BitDo
        return;
    DBG("Remapping 8BitDo Dinput buttons.\n");
    // All 8BitDo controllers in DInput mode have "gaps" in their buttons.
    uint16_t save2 = desc->button_offsets[2];
    uint16_t save5 = desc->button_offsets[5];
    desc->button_offsets[2] = desc->button_offsets[3];
    desc->button_offsets[3] = desc->button_offsets[4];
    for (int i = 4; i <= 9; i++)
        desc->button_offsets[i] = desc->button_offsets[i + 2];
    desc->button_offsets[10] = desc->button_offsets[13];
    desc->button_offsets[11] = desc->button_offsets[14];
    if (product_id == 0x5006) // M30 wired
    {
        // The M30 (Sega) controller has an unsual mapping
        // for the guide button when wired.
        uint16_t save12 = desc->button_offsets[12];
        desc->button_offsets[12] = save2;
        save2 = save12;
    }
    // Drop the gaps at the end, not sure what uses this.
    desc->button_offsets[13] = save2;
    desc->button_offsets[14] = save5;
}

static void des_parse_generic_controller(pad_descriptor_t *desc, uint8_t const *desc_report, uint16_t desc_len)
{
    memset(desc, 0, sizeof(pad_descriptor_t));
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
                break;
            case 0x31: // Y axis (left stick Y)
                desc->y_offset = usage_item.bit_pos;
                desc->y_size = usage_item.size;
                break;
            case 0x32: // Z axis (right stick X)
                desc->z_offset = usage_item.bit_pos;
                desc->z_size = usage_item.size;
                break;
            case 0x35: // Rz axis (right stick Y)
                desc->rz_offset = usage_item.bit_pos;
                desc->rz_size = usage_item.size;
                break;
            case 0x33: // Rx axis (left trigger)
                desc->rx_offset = usage_item.bit_pos;
                desc->rx_size = usage_item.size;
                break;
            case 0x34: // Ry axis (right trigger)
                desc->ry_offset = usage_item.bit_pos;
                desc->ry_size = usage_item.size;
                break;
            case 0x39: // Hat switch (D-pad)
                desc->hat_offset = usage_item.bit_pos;
                desc->hat_size = usage_item.size;
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

void des_report_descriptor(pad_descriptor_t *desc,
                           uint8_t const *desc_report, uint16_t desc_len,
                           uint16_t vendor_id, uint16_t product_id)
{
    DBG("Received HID descriptor. vid=0x%04X, pid=0x%04X, len=%d\n", vendor_id, product_id, desc_len);
    desc->valid = false;

    // Xbox One controllers use XInput protocol
    if (!desc->valid)
    {
        des_xbox_one_controller(desc, vendor_id, product_id);
        if (desc->valid)
            DBG("Detected Xbox One controller, using pre-computed descriptor.\n");
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
