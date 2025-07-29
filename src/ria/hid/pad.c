/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <pico.h>
#include "btstack.h"
#include "tusb_config.h"
#include "hid/pad.h"
#include "hid/des.h"
#include "sys/mem.h"
#include "usb/xin.h"

#define DEBUG_RIA_HID_PAD

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_PAD)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// If you're here to remap HID buttons on a new HID gamepad, create
// a new pad_remap_ function and add it to pad_distill_descriptor().

// This is the report we generate for XRAM.
// Direction bits: 0-up, 1-down, 2-left, 3-right
// Feature bit 0x80 is on when valid controller connected
// Feature bit 0x40 is on when Sony-style controller detected
typedef struct
{
    uint8_t dpad;    // dpad (0x0F) and feature (0xF0) bits
    uint8_t sticks;  // left (0x0F) and right (0xF0) sticks
    uint8_t button0; // buttons
    uint8_t button1; // buttons
    int8_t lx;       // left analog-stick
    int8_t ly;       // left analog-stick
    int8_t rx;       // right analog-stick
    int8_t ry;       // right analog-stick
    uint8_t lt;      // analog left trigger
    uint8_t rt;      // analog right trigger
} pad_report_t;

// Deadzone is generous enough for moderately worn sticks.
// This is only for the analog to digital comversions so
// it doesn't need to be first-person shooter tight.
#define PAD_DEADZONE 24

// Room for button0 and button1 plus a dpad if needed.
#define PAD_MAX_BUTTONS 20

#define PAD_HOME_BUTTON 12

// Gamepad descriptors are normalized to this structure.
typedef struct
{
    bool valid;
    bool sony;         // Indicates gamepad uses sony button labels
    bool home_pressed; // Used to inject the out of band home button on xbox one
    uint8_t slot;      // HID protocol drivers use slots assigned in hid.h
    uint8_t report_id; // If non zero, the first report byte must match and will be skipped
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
    uint16_t button_offsets[PAD_MAX_BUTTONS];
} pad_descriptor_t;

// Where in XRAM to place reports, 0xFFFF when disabled.
static uint16_t pad_xram;

// Parsed descriptor structure for fast report parsing.
static pad_descriptor_t pad_descriptors[PAD_MAX_PLAYERS];

static inline void pad_swap_buttons(pad_descriptor_t *desc, int b0, int b1)
{
    uint16_t temp = desc->button_offsets[b0];
    desc->button_offsets[b0] = desc->button_offsets[b1];
    desc->button_offsets[b1] = temp;
}

// These are USB controllers for the Classic, a remake of the PS1/PSOne.
static void pad_remap_playstation_classic(
    pad_descriptor_t *desc, uint16_t vendor_id, uint16_t product_id)
{
    (void)product_id;
    if (vendor_id != 0x054C) // Sony Interactive Entertainment
        return;
    DBG("Playstation Classic remap: vid=0x%04X, pid=0x%04X\n", vendor_id, product_id);
    desc->sony = true;
    pad_swap_buttons(desc, 0, 2); // buttons
    pad_swap_buttons(desc, 2, 3); // buttons
    pad_swap_buttons(desc, 4, 8); // l1/l2
    pad_swap_buttons(desc, 5, 9); // r1/r2
    pad_swap_buttons(desc, 4, 6); // l1/bt
    pad_swap_buttons(desc, 5, 7); // r1/st
}

// The 8BitDo M30 is a Sega-style gamepad with wonky button mappings.
// It has different L1/R1/L2/R2 mappings for XInput and DInput, which we leave alone.
// Sadly, remapping C/Z into the correct place would mean a confusing third mapping.
// The barrier to a better map is that we can't detect an M30 using a USB Bluetooth adapter.
// The wired DInput mode is unlike any other 8BitDo device so we fix it up here.
static void pad_remap_8bitdo_m30(
    pad_descriptor_t *desc, uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id != 0x2DC8 || product_id != 0x5006)
        return;
    DBG("DES: 8BitDo M30 remap: vid=0x%04X, pid=0x%04X\n", vendor_id, product_id);
    // Our analog trigger emulation conflicts
    // with the M30's reversed analog triggers.
    desc->rx_size = 0;
    desc->ry_size = 0;
    // home is on 2 because reasons
    pad_swap_buttons(desc, 2, PAD_HOME_BUTTON);
}

// Sony DualShock 4 detection
static bool pad_is_sony_ds4(uint16_t vendor_id, uint16_t product_id)
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

// Sony DualSense 5 detection
static bool pad_is_sony_ds5(uint16_t vendor_id, uint16_t product_id)
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

// XBox One/Series descriptor for XInput
static const pad_descriptor_t __in_flash("hid_descriptors") pad_desc_xbox_one = {
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
        0xFFFF,    // C unused
        3 * 8 + 6, // X button
        3 * 8 + 7, // Y button
        0xFFFF,    // Z unused
        4 * 8 + 4, // Left shoulder/LB
        4 * 8 + 5, // Right shoulder/RB
        //
        0xFFFF,    // L2
        0xFFFF,    // R2
        3 * 8 + 3, // View/Select button
        3 * 8 + 2, // Menu/Start button
        0xFFFF,    // Home button
        4 * 8 + 6, // Left stick click
        4 * 8 + 7, // Right stick click
        0xFFFF,    // unused
        //
        4 * 8 + 0, // D-pad Up
        4 * 8 + 1, // D-pad Down
        4 * 8 + 2, // D-pad Left
        4 * 8 + 3, // D-pad Right
    }};

// XBox 360 descriptor for XInput
static const pad_descriptor_t __in_flash("hid_descriptors") pad_desc_xbox_360 = {
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
        0xFFFF,    // C unused
        3 * 8 + 6, // X button
        3 * 8 + 7, // Y button
        0xFFFF,    // Z unused
        3 * 8 + 0, // Left shoulder/LB
        3 * 8 + 1, // Right shoulder/RB
        //
        0xFFFF,    // L2
        0xFFFF,    // R2
        2 * 8 + 5, // Back button
        2 * 8 + 4, // Start button
        3 * 8 + 2, // Home button
        2 * 8 + 6, // Left stick click
        2 * 8 + 7, // Right stick click
        0xFFFF,    // unused
        //
        2 * 8 + 0, // D-pad Up
        2 * 8 + 1, // D-pad Down
        2 * 8 + 2, // D-pad Left
        2 * 8 + 3  // D-pad Right
    }};

// Sony DualShock 4 is HID but presents no descriptor
static const pad_descriptor_t __in_flash("hid_descriptors") pad_desc_sony_ds4 = {
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
        // X, Circle, Unused, Square, Triangle, Unused, L1, R1
        37, 38, 0xFFFF, 36, 39, 0xFFFF, 40, 41,
        // L2, R2, Share, Options, L3, R3, PS, Touchpad
        42, 43, 44, 45, 46, 47, 48, 49,
        // Hat buttons computed from HID hat
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};

// Sony DualSense 5 is HID but presents no descriptor
static const pad_descriptor_t __in_flash("hid_descriptors") pad_desc_sony_ds5 = {
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
        // X, Circle, Unused, Square, Triangle, Unused, L1, R1
        61, 62, 0xFFFF, 60, 63, 0xFFFF, 64, 65,
        // L2, R2, Create, Options, L3, R3, PS, Touchpad
        66, 67, 68, 69, 70, 71, 72, 73,
        // Hat buttons computed from HID hat
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};

static void pad_parse_descriptor(
    pad_descriptor_t *desc, uint8_t const *desc_data, uint16_t desc_len)
{
    memset(desc, 0, sizeof(pad_descriptor_t));
    for (int i = 0; i < PAD_MAX_BUTTONS; i++)
        desc->button_offsets[i] = 0xFFFF;

    // Use BTstack HID parser to parse the descriptor
    btstack_hid_usage_iterator_t iterator;
    btstack_hid_usage_iterator_init(&iterator, desc_data, desc_len, HID_REPORT_TYPE_INPUT);
    while (btstack_hid_usage_iterator_has_more(&iterator))
    {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&iterator, &item);

        // Store report ID if this is the first one we encounter
        if (desc->report_id == 0 && item.report_id != 0xFFFF)
            desc->report_id = item.report_id;

        // Map usages to gamepad fields
        if (item.usage_page == 0x01) // Generic Desktop
        {
            switch (item.usage)
            {
            case 0x30: // X axis (left stick X)
                desc->x_offset = item.bit_pos;
                desc->x_size = item.size;
                desc->x_min = iterator.global_logical_minimum;
                desc->x_max = iterator.global_logical_maximum;
                break;
            case 0x31: // Y axis (left stick Y)
                desc->y_offset = item.bit_pos;
                desc->y_size = item.size;
                desc->y_min = iterator.global_logical_minimum;
                desc->y_max = iterator.global_logical_maximum;
                break;
            case 0x32: // Z axis (right stick X)
                desc->z_offset = item.bit_pos;
                desc->z_size = item.size;
                desc->z_min = iterator.global_logical_minimum;
                desc->z_max = iterator.global_logical_maximum;
                break;
            case 0x35: // Rz axis (right stick Y)
                desc->rz_offset = item.bit_pos;
                desc->rz_size = item.size;
                desc->rz_min = iterator.global_logical_minimum;
                desc->rz_max = iterator.global_logical_maximum;
                break;
            case 0x33: // Rx axis (left trigger)
                desc->rx_offset = item.bit_pos;
                desc->rx_size = item.size;
                desc->rx_min = iterator.global_logical_minimum;
                desc->rx_max = iterator.global_logical_maximum;
                break;
            case 0x34: // Ry axis (right trigger)
                desc->ry_offset = item.bit_pos;
                desc->ry_size = item.size;
                desc->ry_min = iterator.global_logical_minimum;
                desc->ry_max = iterator.global_logical_maximum;
                break;
            case 0x39: // Hat switch (D-pad)
                desc->hat_offset = item.bit_pos;
                desc->hat_size = item.size;
                desc->hat_min = iterator.global_logical_minimum;
                desc->hat_max = iterator.global_logical_maximum;
                break;
            }
        }
        else if (item.usage_page == 0x02) // Simulation Controls
        {
            switch (item.usage)
            {
            case 0xC5: // Brake (left trigger)
                desc->rx_offset = item.bit_pos;
                desc->rx_size = item.size;
                desc->rx_min = iterator.global_logical_minimum;
                desc->rx_max = iterator.global_logical_maximum;
                break;
            case 0xC4: // Accelerator (right trigger)
                desc->ry_offset = item.bit_pos;
                desc->ry_size = item.size;
                desc->ry_min = iterator.global_logical_minimum;
                desc->ry_max = iterator.global_logical_maximum;
                break;
            }
        }
        else if (item.usage_page == 0x09) // Button page
        {
            uint8_t button_index = item.usage - 1; // Buttons 1-indexed
            if (button_index < PAD_MAX_BUTTONS)
                desc->button_offsets[button_index] = item.bit_pos;
        }
    }

    // If it quacks like a joystick
    if (desc->x_size || desc->y_size || desc->z_size ||
        desc->rz_size || desc->rx_size || desc->ry_size ||
        desc->hat_size || desc->button_offsets[0] != 0xFFFF)
        desc->valid = true;
}

static void pad_distill_descriptor(
    uint8_t slot, pad_descriptor_t *desc,
    uint8_t const *desc_data, uint16_t desc_len,
    uint16_t vendor_id, uint16_t product_id)
{
    desc->valid = false;
    pad_parse_descriptor(desc, desc_data, desc_len);

    DBG("Received HID descriptor. vid=0x%04X, pid=0x%04X, len=%d, valid=%d\n",
        vendor_id, product_id, desc_len, desc->valid);

    // Add your gamepad override here.
    pad_remap_8bitdo_m30(desc, vendor_id, product_id);
    pad_remap_playstation_classic(desc, vendor_id, product_id);

    // Non HID controllers use a pre-computed descriptor
    if (desc_len == 0)
    {
        if (xin_is_xbox_one(slot))
        {
            *desc = pad_desc_xbox_one;
            DBG("Detected Xbox One controller, using pre-computed descriptor.\n");
        }
        if (xin_is_xbox_360(slot))
        {
            *desc = pad_desc_xbox_360;
            DBG("Detected Xbox 360 controller, using pre-computed descriptor.\n");
        }
        if (pad_is_sony_ds4(vendor_id, product_id))
        {
            *desc = pad_desc_sony_ds4;
            DBG("Detected Sony DS4 controller, using pre-computed descriptor.\n");
        }
        if (pad_is_sony_ds5(vendor_id, product_id))
        {
            *desc = pad_desc_sony_ds5;
            DBG("Detected Sony DS5 controller, using pre-computed descriptor.\n");
        }
    }

    if (!desc->valid)
        DBG("HID descriptor not a gamepad.\n");
}

static uint32_t pad_extract_bits(const uint8_t *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size)
{
    if (!bit_size || bit_size > 32)
        return 0;

    uint16_t start_byte = bit_offset / 8;
    uint8_t start_bit = bit_offset % 8;
    uint16_t end_byte = (bit_offset + bit_size - 1) / 8;

    if (end_byte >= report_len)
        return 0;

    // Extract up to 4 bytes into a 32-bit value
    uint32_t value = 0;
    for (uint8_t i = 0; i < 4 && (start_byte + i) < report_len; ++i)
        value |= ((uint32_t)report[start_byte + i]) << (8 * i);

    value >>= start_bit;
    if (bit_size < 32)
        value &= (1UL << bit_size) - 1;

    return value;
}

static uint8_t pad_scale_analog(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max)
{
    // Handle reversed polarity
    bool reversed = logical_min > logical_max;
    int32_t min = reversed ? logical_max : logical_min;
    int32_t max = reversed ? logical_min : logical_max;

    // Sign-extend raw_value if needed
    int32_t value = (int32_t)raw_value;
    if (min < 0 && bit_size < 32)
    {
        uint32_t sign_bit = 1UL << (bit_size - 1);
        if (raw_value & sign_bit)
            value |= ~((1UL << bit_size) - 1);
    }

    // Clamp to logical range
    if (value < min)
        value = min;
    if (value > max)
        value = max;

    // Scale to 0-255
    int32_t range = max - min;
    if (range == 0)
        return 127;
    uint8_t result = (uint8_t)(((value - min) * 255) / range);

    // Reverse if needed
    if (reversed)
        result = 255 - result;

    return result;
}

static int8_t pad_scale_analog_signed(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max)
{
    // Handle reversed polarity
    bool reversed = logical_min > logical_max;
    int32_t min = reversed ? logical_max : logical_min;
    int32_t max = reversed ? logical_min : logical_max;

    // Sign-extend raw_value if needed
    int32_t value = (int32_t)raw_value;
    if (min < 0 && bit_size < 32)
    {
        uint32_t sign_bit = 1UL << (bit_size - 1);
        if (raw_value & sign_bit)
            value |= ~((1UL << bit_size) - 1);
    }

    // Clamp to logical range
    if (value < min)
        value = min;
    if (value > max)
        value = max;

    int32_t range = max - min;
    if (range == 0)
        return 0;

    // Scale to -128..127, ensuring 256 values
    // Map min to -128, max to 127
    int32_t scaled = ((value - min) * 255 + (range / 2)) / range - 128;
    int8_t result = (int8_t)scaled;

    // Reverse if needed
    if (reversed)
        result = -result - 1;

    return result;
}

static uint8_t pad_encode_stick(int8_t x, int8_t y)
{
    // Deadzone check
    if (x >= -PAD_DEADZONE && x <= PAD_DEADZONE &&
        y >= -PAD_DEADZONE && y <= PAD_DEADZONE)
        return 0; // No direction

    // Get absolute values
    int16_t abs_x = (x < 0) ? -x : x;
    int16_t abs_y = (y < 0) ? -y : y;

    // Use a 2:1 ratio to distinguish cardinal from diagonal
    if (abs_y >= (abs_x * 2))
        return (y < 0) ? 1 : 2; // North : South
    if (abs_x >= (abs_y * 2))
        return (x < 0) ? 4 : 8; // West : East

    // Mixed movement - diagonal
    uint8_t result = 0;
    // Vertical component
    if (y < 0)
        result |= 1; // North
    else
        result |= 2; // South
    // Horizontal component
    if (x < 0)
        result |= 4; // West
    else
        result |= 8; // East

    return result;
}

static int pad_find_player_by_slot(uint8_t slot)
{
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
        if (pad_descriptors[i].slot == slot && pad_descriptors[i].valid)
            return i;
    return -1;
}

static void pad_parse_report(int player, uint8_t const *data, uint16_t report_len, pad_report_t *report)
{
    // Default empty gamepad report
    memset(report, 0, sizeof(pad_report_t));

    // Add feature bits to dpad
    pad_descriptor_t *gamepad = &pad_descriptors[player];
    if (gamepad->valid)
        report->dpad |= 0x80;
    if (gamepad->sony)
        report->dpad |= 0x40;

    // A blank report was requested
    if (report_len == 0)
        return;

    // Extract analog sticks
    if (gamepad->x_size > 0)
    {
        uint32_t raw_x = pad_extract_bits(data, report_len, gamepad->x_offset, gamepad->x_size);
        report->lx = pad_scale_analog_signed(raw_x, gamepad->x_size, gamepad->x_min, gamepad->x_max);
    }
    if (gamepad->y_size > 0)
    {
        uint32_t raw_y = pad_extract_bits(data, report_len, gamepad->y_offset, gamepad->y_size);
        report->ly = pad_scale_analog_signed(raw_y, gamepad->y_size, gamepad->y_min, gamepad->y_max);
    }
    if (gamepad->z_size > 0)
    {
        uint32_t raw_z = pad_extract_bits(data, report_len, gamepad->z_offset, gamepad->z_size);
        report->rx = pad_scale_analog_signed(raw_z, gamepad->z_size, gamepad->z_min, gamepad->z_max);
    }
    if (gamepad->rz_size > 0)
    {
        uint32_t raw_rz = pad_extract_bits(data, report_len, gamepad->rz_offset, gamepad->rz_size);
        report->ry = pad_scale_analog_signed(raw_rz, gamepad->rz_size, gamepad->rz_min, gamepad->rz_max);
    }

    // Extract triggers
    if (gamepad->rx_size > 0)
    {
        uint32_t raw_rx = pad_extract_bits(data, report_len, gamepad->rx_offset, gamepad->rx_size);
        report->lt = pad_scale_analog(raw_rx, gamepad->rx_size, gamepad->rx_min, gamepad->rx_max);
    }
    if (gamepad->ry_size > 0)
    {
        uint32_t raw_ry = pad_extract_bits(data, report_len, gamepad->ry_offset, gamepad->ry_size);
        report->rt = pad_scale_analog(raw_ry, gamepad->ry_size, gamepad->ry_min, gamepad->ry_max);
    }

    // Extract buttons using individual bit offsets
    uint32_t buttons = 0;
    for (int i = 0; i < PAD_MAX_BUTTONS; i++)
        if (pad_extract_bits(data, report_len, gamepad->button_offsets[i], 1))
            buttons |= (1UL << i);
    report->button0 = buttons & 0xFF;
    report->button1 = (buttons & 0xFF00) >> 8;

    // Extract D-pad/hat
    if (gamepad->hat_size == 4 && gamepad->hat_max - gamepad->hat_min == 7)
    {
        // Convert HID hat format to individual direction bits
        static const uint8_t hat_to_pad[] = {1, 9, 8, 10, 2, 6, 4, 5};
        uint32_t raw_hat = pad_extract_bits(data, report_len, gamepad->hat_offset, gamepad->hat_size);
        unsigned index = raw_hat - gamepad->hat_min;
        if (index < 8)
            report->dpad |= hat_to_pad[index];
    }
    else
    {
        // Look for xbone-style discrete dpad buttons in 16-19
        report->dpad |= (buttons & 0xF0000) >> 16;
    }

    // Generate dpad values for sticks
    uint8_t stick_l = pad_encode_stick(report->lx, report->ly);
    uint8_t stick_r = pad_encode_stick(report->rx, report->ry);
    report->sticks = stick_l | (stick_r << 4);

    // If L2/R2 buttons pressed without any analog movement
    if ((buttons & (1 << 8)) && (report->lt == 0))
        report->lt = 255;
    if ((buttons & (1 << 9)) && (report->rt == 0))
        report->rt = 255;

    // Inject Xbox One home button
    if (gamepad->home_pressed)
        report->button1 |= (1 << (PAD_HOME_BUTTON - 8));

    // If L2/R2 analog movement, ensure button press
    if (report->lt > PAD_DEADZONE)
        report->button1 |= (1 << 0); // L2
    if (report->rt > PAD_DEADZONE)
        report->button1 |= (1 << 1); // R2
}

void pad_init(void)
{
    pad_stop();
}

void pad_stop(void)
{
    pad_xram = 0xFFFF;
}

// Provides first and final updates in xram
static void pad_reset_xram(int player)
{
    if (pad_xram == 0xFFFF)
        return;
    pad_report_t gamepad_report;
    pad_parse_report(player, 0, 0, &gamepad_report); // get blank
    memcpy(&xram[pad_xram + player * (sizeof(pad_report_t))],
           &gamepad_report, sizeof(pad_report_t));
}

bool pad_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - (sizeof(pad_report_t)) * PAD_MAX_PLAYERS)
        return false;
    pad_xram = word;
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
        pad_reset_xram(i);
    return true;
}

bool pad_mount(uint8_t slot, uint8_t const *desc_data, uint16_t desc_len,
               uint16_t vendor_id, uint16_t product_id)
{
    pad_descriptor_t *gamepad = NULL;
    int player;
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
    {
        if (!pad_descriptors[i].valid)
        {
            gamepad = &pad_descriptors[i];
            player = i;
            break;
        }
    }
    if (!gamepad)
    {
        DBG("pad_mount: No available descriptor slots, max players reached\n");
        return false;
    }
    DBG("pad_mount: mounting player %d\n", player);

    pad_distill_descriptor(slot, gamepad, desc_data, desc_len,
                           vendor_id, product_id);
    if (gamepad->valid)
    {
        gamepad->slot = slot;
        pad_reset_xram(player);
        return true;
    }
    return false;
}

void pad_umount(uint8_t slot)
{
    // Find the descriptor by dev_addr and slot
    int player = pad_find_player_by_slot(slot);
    if (player < 0)
        return;
    pad_descriptor_t *gamepad = &pad_descriptors[player];
    gamepad->valid = false;
    gamepad->slot = 0;
    pad_reset_xram(player);
}

void pad_report(uint8_t slot, uint8_t const *data, uint16_t len)
{
    int player = pad_find_player_by_slot(slot);
    if (player < 0)
        return;
    pad_descriptor_t *descriptor = &pad_descriptors[player];

    // Skip report ID check if no report ID is expected,
    // or validate if one is expected
    const uint8_t *report_data = data;
    uint16_t report_data_len = len;
    if (descriptor->report_id != 0)
    {
        if (len == 0 || data[0] != descriptor->report_id)
            return;
        // Skip report ID byte
        report_data = &data[1];
        report_data_len = len - 1;
    }

    // Parse report and send it to xram
    if (pad_xram != 0xFFFF)
    {
        pad_report_t gamepad_report;
        pad_parse_report(player, report_data, report_data_len, &gamepad_report);
        memcpy(&xram[pad_xram + player * (sizeof(pad_report_t))],
               &gamepad_report, sizeof(pad_report_t));
    }
}

bool pad_is_valid(uint8_t slot)
{
    return pad_find_player_by_slot(slot) >= 0;
}

// This is for XBox One/Series gamepads which send
// the home button down a different path.
void pad_home_button(uint8_t slot, bool pressed)
{
    int player = pad_find_player_by_slot(slot);
    if (player < 0)
        return;
    pad_descriptor_t *gamepad = &pad_descriptors[player];

    // Inject out of band home button into reports
    gamepad->home_pressed = pressed;

    // Update the home button bit in xram
    if (pad_xram != 0xFFFF)
    {
        uint8_t *button1 = &xram[pad_xram + player * (sizeof(pad_report_t)) + 3];
        if (pressed)
            *button1 |= (1 << (PAD_HOME_BUTTON - 8));
        else
            *button1 &= ~(1 << (PAD_HOME_BUTTON - 8));
    }
}

// Useful for gamepads that indicate player number.
int pad_get_player_num(uint8_t slot)
{
    return pad_find_player_by_slot(slot);
}
