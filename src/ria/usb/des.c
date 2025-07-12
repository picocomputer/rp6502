/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "des.h"
#include "btstack.h"
#include "btstack_hid.h"
#include <string.h>

#define DEBUG_RIA_USB_DES /////////////////////////////

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_DES)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...) \
    {            \
    }
#endif

// Pre-computed Sony controller descriptors
static const pad_descriptor_t ds4_descriptor = {
    .valid = true,
    .dev_addr = 0, // Will be set dynamically
    .report_id = 1, // DS4 uses report ID 1 for input reports
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
    .buttons_offset = 4 * 8 + 4, // Byte 4 upper nibble + bytes 5-6
    .buttons_size = 14}; // 4 bits (byte 4 upper) + 8 bits (byte 5) + 4 bits (byte 6 lower)

static const pad_descriptor_t ds5_descriptor = {
    .valid = true,
    .dev_addr = 0, // Will be set dynamically
    .report_id = 1, // DualSense uses report ID 1 for input reports
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
    .buttons_offset = 7 * 8 + 4, // Byte 7 upper nibble + byte 8
    .buttons_size = 14};

// Function to detect Sony controller type
static const pad_descriptor_t *detect_sony_controller(uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id == 0x054C) // Sony Interactive Entertainment
    {
        switch (product_id)
        {
        case 0x05C4: // DualShock 4 Controller
        case 0x09CC: // DualShock 4 Controller (2nd generation)
        case 0x0BA0: // DualShock 4 USB receiver
            return &ds4_descriptor;
        case 0x0CE6: // DualSense Controller
        case 0x0DF2: // DualSense Edge Controller
            return &ds5_descriptor;
        }
    }

    // Third-party PlayStation-compatible controllers
    if (vendor_id == 0x0F0D) // Hori
    {
        switch (product_id)
        {
        case 0x00EE: // Hori Real Arcade Pro 4 Kai (PS4)
        case 0x011C: // Hori Fighting Commander (PS4)
            return &ds4_descriptor;
        }
    }

    if (vendor_id == 0x146B) // BigBen/Nacon
    {
        switch (product_id)
        {
        case 0x0D01: // Nacon Revolution Pro Controller
        case 0x0D02: // Nacon Revolution Pro Controller 2
            return &ds4_descriptor;
        }
    }

    return NULL; // Not a Sony controller
}

bool des_parse_report_descriptor(pad_descriptor_t *descriptors, uint8_t max_descriptors,
                                 uint8_t dev_addr, uint8_t const *desc_report, uint16_t desc_len,
                                 uint16_t vendor_id, uint16_t product_id)
{
    // Find an existing descriptor for this device or allocate a new one
    pad_descriptor_t *desc = NULL;

    // First, check if we already have a descriptor for this device
    for (uint8_t i = 0; i < max_descriptors; i++)
    {
        if (descriptors[i].valid && descriptors[i].dev_addr == dev_addr)
        {
            desc = &descriptors[i];
            break;
        }
    }

    // If not found, find an empty slot
    if (!desc)
    {
        for (uint8_t i = 0; i < max_descriptors; i++)
        {
            if (!descriptors[i].valid)
            {
                desc = &descriptors[i];
                break;
            }
        }
    }

    if (!desc)
        return false;

    DBG("des_parse_report_descriptor: dev_addr=%d, vid=0x%04X, pid=0x%04X, desc_len=%d\n",
        dev_addr, vendor_id, product_id, desc_len);

    // Check if this is a known Sony controller
    const pad_descriptor_t *sony_desc = detect_sony_controller(vendor_id, product_id);
    if (sony_desc)
    {
        DBG("Detected Sony controller, using pre-computed descriptor\n");
        *desc = *sony_desc;        // Copy the pre-computed descriptor
        desc->dev_addr = dev_addr; // Set the device address

        DBG("Sony controller descriptor loaded:\n");
        DBG("  Report ID: %d\n", desc->report_id);
        DBG("  X: offset=%d, size=%d\n", desc->x_offset, desc->x_size);
        DBG("  Y: offset=%d, size=%d\n", desc->y_offset, desc->y_size);
        DBG("  Z: offset=%d, size=%d\n", desc->z_offset, desc->z_size);
        DBG("  Rz: offset=%d, size=%d\n", desc->rz_offset, desc->rz_size);
        DBG("  Rx: offset=%d, size=%d\n", desc->rx_offset, desc->rx_size);
        DBG("  Ry: offset=%d, size=%d\n", desc->ry_offset, desc->ry_size);
        DBG("  Hat: offset=%d, size=%d\n", desc->hat_offset, desc->hat_size);
        DBG("  Buttons: offset=%d, size=%d\n", desc->buttons_offset, desc->buttons_size);

        return true;
    }

    DBG("Unknown controller (not Sony), parsing HID descriptor\n");

    // Initialize descriptor
    memset(desc, 0, sizeof(pad_descriptor_t));
    desc->valid = true;
    desc->dev_addr = dev_addr;

    // Use BTstack HID parser to parse the descriptor
    btstack_hid_usage_iterator_t usage_iterator;
    btstack_hid_usage_iterator_init(&usage_iterator, desc_report, desc_len, HID_REPORT_TYPE_INPUT);

    // Iterate through all input usages to find gamepad controls
    while (btstack_hid_usage_iterator_has_more(&usage_iterator))
    {
        btstack_hid_usage_item_t usage_item;
        btstack_hid_usage_iterator_get_item(&usage_iterator, &usage_item);

        DBG("  Usage: page=0x%04X, usage=0x%04X, bit_pos=%d, size=%d, report_id=%d\n",
            usage_item.usage_page, usage_item.usage, usage_item.bit_pos, usage_item.size, usage_item.report_id);

        // Store report ID if this is the first one we encounter
        if (desc->report_id == 0 && usage_item.report_id != 0xFFFF)
        {
            desc->report_id = usage_item.report_id;
        }

        // Map usages to gamepad fields
        if (usage_item.usage_page == 0x01) // Generic Desktop
        {
            switch (usage_item.usage)
            {
            case 0x30: // X axis (left stick X)
                desc->x_offset = usage_item.bit_pos;
                desc->x_size = usage_item.size;
                DBG("Found X axis at bit %d, size %d\n", usage_item.bit_pos, usage_item.size);
                break;
            case 0x31: // Y axis (left stick Y)
                desc->y_offset = usage_item.bit_pos;
                desc->y_size = usage_item.size;
                DBG("Found Y axis at bit %d, size %d\n", usage_item.bit_pos, usage_item.size);
                break;
            case 0x32: // Z axis (right stick X)
                desc->z_offset = usage_item.bit_pos;
                desc->z_size = usage_item.size;
                DBG("Found Z axis at bit %d, size %d\n", usage_item.bit_pos, usage_item.size);
                break;
            case 0x35: // Rz axis (right stick Y)
                desc->rz_offset = usage_item.bit_pos;
                desc->rz_size = usage_item.size;
                DBG("Found Rz axis at bit %d, size %d\n", usage_item.bit_pos, usage_item.size);
                break;
            case 0x33: // Rx axis (left trigger)
                desc->rx_offset = usage_item.bit_pos;
                desc->rx_size = usage_item.size;
                DBG("Found Rx axis at bit %d, size %d\n", usage_item.bit_pos, usage_item.size);
                break;
            case 0x34: // Ry axis (right trigger)
                desc->ry_offset = usage_item.bit_pos;
                desc->ry_size = usage_item.size;
                DBG("Found Ry axis at bit %d, size %d\n", usage_item.bit_pos, usage_item.size);
                break;
            case 0x39: // Hat switch (D-pad)
                desc->hat_offset = usage_item.bit_pos;
                desc->hat_size = usage_item.size;
                DBG("Found hat switch at bit %d, size %d\n", usage_item.bit_pos, usage_item.size);
                break;
            }
        }
        else if (usage_item.usage_page == 0x09) // Button page
        {
            // For buttons, we want to track the first button's position and total button count
            if (desc->buttons_size == 0 || usage_item.bit_pos < desc->buttons_offset)
            {
                desc->buttons_offset = usage_item.bit_pos;
            }
            // Calculate the total button size by finding the highest button position
            uint8_t button_end = usage_item.bit_pos + usage_item.size;
            uint8_t current_end = desc->buttons_offset + desc->buttons_size;
            if (button_end > current_end)
            {
                desc->buttons_size = button_end - desc->buttons_offset;
            }
            DBG("Found button at bit %d, size %d (total buttons area: offset=%d, size=%d)\n",
                usage_item.bit_pos, usage_item.size, desc->buttons_offset, desc->buttons_size);
        }
    }

    DBG("BTstack parsing complete. Final descriptor:\n");
    DBG("  Report ID: %d\n", desc->report_id);
    DBG("  X: offset=%d, size=%d\n", desc->x_offset, desc->x_size);
    DBG("  Y: offset=%d, size=%d\n", desc->y_offset, desc->y_size);
    DBG("  Z: offset=%d, size=%d\n", desc->z_offset, desc->z_size);
    DBG("  Rz: offset=%d, size=%d\n", desc->rz_offset, desc->rz_size);
    DBG("  Rx: offset=%d, size=%d\n", desc->rx_offset, desc->rx_size);
    DBG("  Ry: offset=%d, size=%d\n", desc->ry_offset, desc->ry_size);
    DBG("  Hat: offset=%d, size=%d\n", desc->hat_offset, desc->hat_size);
    DBG("  Buttons: offset=%d, size=%d\n", desc->buttons_offset, desc->buttons_size);

    return true;
}
