/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "des.h"
#include "btstack.h"
#include <string.h>

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_DES)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...) \
    {            \
    }
#endif

static const pad_descriptor_t *des_sony_ds4_controller(uint16_t vendor_id, uint16_t product_id)
{
    static const pad_descriptor_t ds4_descriptor = {
        .valid = true,
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
            // DS4 button layout: X, Circle, Square, Triangle, L1, R1, L2, R2, Share, Options, PS, L3, R3, Touchpad
            37, 38, 36, 39, 40, 41, 42, 43, 44, 45, 48, 46, 47, 49,
            // Mark unused buttons with 0xFFFF
            0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};

    if (vendor_id == 0x054C) // Sony Interactive Entertainment
    {
        switch (product_id)
        {
        case 0x05C4: // DualShock 4 Controller
        case 0x09CC: // DualShock 4 Controller (2nd generation)
        case 0x0BA0: // DualShock 4 USB receiver
            return &ds4_descriptor;
        }
    }
    if (vendor_id == 0x0C12) // Zeroplus/Cirka
    {
        switch (product_id)
        {
        case 0x1E1A: // Cirka Wired Controller
            return &ds4_descriptor;
        }
    }
    return NULL;
}

static const pad_descriptor_t *des_sony_ds5_controller(uint16_t vendor_id, uint16_t product_id)
{
    static const pad_descriptor_t ds5_descriptor = {
        .valid = true,
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
            // DS5 button layout: X, Circle, Square, Triangle, L1, R1, L2, R2, Create, Options, PS, L3, R3, Touchpad
            61, 62, 60, 63, 64, 65, 66, 67, 68, 69, 72, 70, 71, 73,
            // Mark unused buttons with 0xFFFF
            0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};

    if (vendor_id == 0x054C) // Sony Interactive Entertainment
    {
        switch (product_id)
        {
        case 0x0CE6: // DualSense Controller
        case 0x0DF2: // DualSense Edge Controller
            return &ds5_descriptor;
        }
    }
    return NULL;
}

static void des_remap_8bitdo_dinput(pad_descriptor_t *desc, uint16_t vendor_id, uint16_t product_id)
{
    (void)product_id;
    if (vendor_id != 0x2DC8) // 8BitDo
        return;
    DBG("Remapping 8BitDo Dinput buttons.\n");
    // All 8BitDo controllers in dinput mode have "gaps" in their buttons.
    uint16_t save2 = desc->button_offsets[2];
    uint16_t save5 = desc->button_offsets[5];
    desc->button_offsets[2] = desc->button_offsets[3];
    desc->button_offsets[3] = desc->button_offsets[4];
    for (int i = 4; i <= 12; i++)
        desc->button_offsets[i] = desc->button_offsets[i + 2];
    if (product_id == 0x5006) // M30 wired
    {
        // The M30 (Sega) controller has an unsual mapping
        // for the guide button when wired.
        uint16_t save10 = desc->button_offsets[10];
        desc->button_offsets[10] = save2;
        save2 = save10;
    }
    // Drops the gaps at the end, not sure what uses this.
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
            uint16_t count = usage_item.size;
            uint16_t bit_pos = usage_item.bit_pos;
            uint8_t button_index = usage_item.usage - 1; // Buttons 1-indexed
            while (count-- && button_index < PAD_MAX_BUTTONS)
                desc->button_offsets[button_index++] = bit_pos++;
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
    pad_descriptor_t const *found;
    desc->valid = false;

    // Sony DualShock 4 controllers don't have a descriptor
    if ((found = des_sony_ds4_controller(vendor_id, product_id)))
    {
        DBG("Detected Sony DS4 controller, using pre-computed descriptor.\n");
        *desc = *found;
    }

    // Sony DualShock 5 controllers don't have a descriptor
    if ((found = des_sony_ds5_controller(vendor_id, product_id)))
    {
        DBG("Detected Sony DS5 controller, using pre-computed descriptor.\n");
        *desc = *found;
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
