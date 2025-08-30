/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hid/hid.h"
#include "hid/pad.h"
#include "sys/mem.h"
#include <btstack_hid_parser.h>
#include <pico.h>
#include <string.h>

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
// Feature bit 0x80 is on when valid gamepad connected
// Feature bit 0x40 is on when Sony-style gamepad detected
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
} pad_xram_t;

#define PAD_MAX_PLAYERS 4

// Deadzone is generous enough for moderately worn sticks.
// This is only for the analog to digital comversions so
// it doesn't need to be first-person shooter tight.
#define PAD_DEADZONE 32

// Room for button0 and button1 plus a dpad if needed.
#define PAD_MAX_BUTTONS 20

#define PAD_HOME_BUTTON 12

// Gamepad descriptors are normalized to this structure.
typedef struct
{
    bool valid;
    bool sony;           // Indicates gamepad uses sony button labels
    bool home_pressed;   // Used to inject the out of band home button on xbox one
    int slot;            // HID protocol drivers use slots assigned in hid.h
    uint8_t report_id;   // If non zero, the first report byte must match and will be skipped
    uint16_t x_absolute; // Will be true for gamepads
    uint16_t x_offset;   // Left stick X
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
} pad_connection_t;

// Where in XRAM to place reports, 0xFFFF when disabled.
static uint16_t pad_xram;

// Parsed descriptor structure for fast report parsing.
static pad_connection_t pad_connections[PAD_MAX_PLAYERS];

static inline void pad_swap_buttons(pad_connection_t *conn, int b0, int b1)
{
    uint16_t temp = conn->button_offsets[b0];
    conn->button_offsets[b0] = conn->button_offsets[b1];
    conn->button_offsets[b1] = temp;
}

// These are USB gamepads for the Classic, a remake of the PS1/PSOne.
static void pad_remap_playstation_classic(
    pad_connection_t *conn, uint16_t vendor_id, uint16_t product_id)
{
    (void)product_id;
    if (vendor_id != 0x054C) // Sony Interactive Entertainment
        return;
    DBG("Playstation Classic remap: vid=0x%04X, pid=0x%04X\n", vendor_id, product_id);
    conn->sony = true;
    pad_swap_buttons(conn, 0, 2); // buttons
    pad_swap_buttons(conn, 2, 3); // buttons
    pad_swap_buttons(conn, 4, 8); // l1/l2
    pad_swap_buttons(conn, 5, 9); // r1/r2
    pad_swap_buttons(conn, 4, 6); // l1/bt
    pad_swap_buttons(conn, 5, 7); // r1/st
}

// The 8BitDo M30 is a Sega-style gamepad with wonky button mappings.
// It has different L1/R1/L2/R2 mappings for XInput and DInput, which we leave alone.
// Sadly, remapping C/Z into the correct place would mean a confusing third mapping.
// The barrier to a better map is that we can't detect an M30 using a USB Bluetooth adapter.
// The wired DInput mode is unlike any other 8BitDo device so we fix it up here.
static void pad_remap_8bitdo_m30(
    pad_connection_t *conn, uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id != 0x2DC8 || product_id != 0x5006)
        return;
    DBG("DES: 8BitDo M30 remap: vid=0x%04X, pid=0x%04X\n", vendor_id, product_id);
    // Our analog trigger emulation conflicts
    // with the M30's reversed analog triggers.
    conn->rx_size = 0;
    conn->ry_size = 0;
    // home is on 2 because reasons
    pad_swap_buttons(conn, 2, PAD_HOME_BUTTON);
}

// Sony DualShock 4 detection
static bool pad_is_sony_ds4(uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id == 0x054C) // Sony Interactive Entertainment
    {
        switch (product_id)
        {
        case 0x05C4: // DualShock 4 (1st gen)
        case 0x09CC: // DualShock 4 (2nd gen)
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
        case 0x1E1A: // Cirka Wired
        case 0x0E10: // Zeroplus PS4 compatible
        case 0x0E20: // Zeroplus PS4 compatible
            return true;
        }
    }
    if (vendor_id == 0x20D6) // PowerA
    {
        switch (product_id)
        {
        case 0xA711: // PowerA PS4 Wired
            return true;
        }
    }
    if (vendor_id == 0x24C6) // PowerA (formerly BDA, LLC)
    {
        switch (product_id)
        {
        case 0x5501: // PowerA PS4 Wired
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
        case 0x0CE6: // DualSense
        case 0x0DF2: // DualSense Edge
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

// Sony DualShock 4 is HID but presents no descriptor
static const pad_connection_t pad_desc_sony_ds4 = {
    .valid = true,
    .x_absolute = true,
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
        // L2, R2, Share, Options, PS, L3, R3, Touchpad
        42, 43, 44, 45, 48, 46, 47, 49,
        // Hat buttons computed from HID hat
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};

// Sony DualSense 5 is HID but presents no descriptor
static const pad_connection_t pad_desc_sony_ds5 = {
    .valid = true,
    .x_absolute = true,
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
        // L2, R2, Create, Options, PS, L3, R3, Touchpad
        66, 67, 68, 69, 72, 70, 71, 73,
        // Hat buttons computed from HID hat
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};

static void pad_parse_descriptor(
    pad_connection_t *conn, uint8_t const *desc_data, uint16_t desc_len)
{
    memset(conn, 0, sizeof(pad_connection_t));
    for (int i = 0; i < PAD_MAX_BUTTONS; i++)
        conn->button_offsets[i] = 0xFFFF;

    DBG("Raw HID descriptor (%d bytes):\n", desc_len);
    for (int i = 0; i < desc_len; i++)
    {
        DBG("%02X ", desc_data[i]);
        if ((i + 1) % 26 == 0)
            DBG("\n");
    }
    if (desc_len % 26 != 0)
        DBG("\n");

    // Use BTstack HID parser to parse the descriptor
    btstack_hid_usage_iterator_t iterator;
    btstack_hid_usage_iterator_init(&iterator, desc_data, desc_len, HID_REPORT_TYPE_INPUT);
    while (btstack_hid_usage_iterator_has_more(&iterator))
    {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&iterator, &item);

        // Log each HID usage item
        // DBG("HID item: usage_page=0x%02x, usage=0x%02x, report_id=0x%04x\n",
        //     item.usage_page, item.usage, item.report_id);

        bool get_report_id = false;
        if (item.usage_page == 0x01) // Generic Desktop
        {
            get_report_id = true;
            switch (item.usage)
            {
            case 0x30: // X axis (left stick X)
                conn->x_offset = item.bit_pos;
                conn->x_size = item.size;
                conn->x_min = iterator.global_logical_minimum;
                conn->x_max = iterator.global_logical_maximum;
                conn->x_absolute = !(iterator.descriptor_item.item_value & 0x04);
                break;
            case 0x31: // Y axis (left stick Y)
                conn->y_offset = item.bit_pos;
                conn->y_size = item.size;
                conn->y_min = iterator.global_logical_minimum;
                conn->y_max = iterator.global_logical_maximum;
                break;
            case 0x32: // Z axis (right stick X)
                conn->z_offset = item.bit_pos;
                conn->z_size = item.size;
                conn->z_min = iterator.global_logical_minimum;
                conn->z_max = iterator.global_logical_maximum;
                break;
            case 0x35: // Rz axis (right stick Y)
                conn->rz_offset = item.bit_pos;
                conn->rz_size = item.size;
                conn->rz_min = iterator.global_logical_minimum;
                conn->rz_max = iterator.global_logical_maximum;
                break;
            case 0x33: // Rx axis (left trigger)
                conn->rx_offset = item.bit_pos;
                conn->rx_size = item.size;
                conn->rx_min = iterator.global_logical_minimum;
                conn->rx_max = iterator.global_logical_maximum;
                break;
            case 0x34: // Ry axis (right trigger)
                conn->ry_offset = item.bit_pos;
                conn->ry_size = item.size;
                conn->ry_min = iterator.global_logical_minimum;
                conn->ry_max = iterator.global_logical_maximum;
                break;
            case 0x39: // Hat switch (D-pad)
                conn->hat_offset = item.bit_pos;
                conn->hat_size = item.size;
                conn->hat_min = iterator.global_logical_minimum;
                conn->hat_max = iterator.global_logical_maximum;
                break;
            }
        }
        else if (item.usage_page == 0x02) // Simulation Controls
        {
            get_report_id = true;
            switch (item.usage)
            {
            case 0xC5: // Brake (left trigger)
                conn->rx_offset = item.bit_pos;
                conn->rx_size = item.size;
                conn->rx_min = iterator.global_logical_minimum;
                conn->rx_max = iterator.global_logical_maximum;
                break;
            case 0xC4: // Accelerator (right trigger)
                conn->ry_offset = item.bit_pos;
                conn->ry_size = item.size;
                conn->ry_min = iterator.global_logical_minimum;
                conn->ry_max = iterator.global_logical_maximum;
                break;
            }
        }
        else if (item.usage_page == 0x09) // Button page
        {
            get_report_id = true;
            uint8_t button_index = item.usage - 1; // Buttons 1-indexed
            if (button_index < PAD_MAX_BUTTONS)
                conn->button_offsets[button_index] = item.bit_pos;
        }
        // Store report ID if this is the first one we encounter
        if (get_report_id && conn->report_id == 0 && item.report_id != 0xFFFF)
            conn->report_id = item.report_id;
    }

    // If it creaks like a gamepad.
    if (conn->x_absolute && conn->button_offsets[0] != 0xFFFF &&
        (conn->x_size || conn->y_size || conn->z_size ||
         conn->rz_size || conn->rx_size || conn->ry_size ||
         conn->hat_size))
        conn->valid = true;

    // Debug: Print parsed pad_connection_t structure
    DBG("Parsed pad_connection_t:\n");
    DBG("  valid=%d, sony=%d, slot=%d, report_id=0x%02X\n",
        conn->valid, conn->sony, conn->slot, conn->report_id);
    DBG("  x_absolute=%d, x_offset=%d, x_size=%d, x_min=%ld, x_max=%ld\n",
        conn->x_absolute, conn->x_offset, conn->x_size, conn->x_min, conn->x_max);
    DBG("  y_offset=%d, y_size=%d, y_min=%ld, y_max=%ld\n",
        conn->y_offset, conn->y_size, conn->y_min, conn->y_max);
    DBG("  z_offset=%d, z_size=%d, z_min=%ld, z_max=%ld\n",
        conn->z_offset, conn->z_size, conn->z_min, conn->z_max);
    DBG("  rz_offset=%d, rz_size=%d, rz_min=%ld, rz_max=%ld\n",
        conn->rz_offset, conn->rz_size, conn->rz_min, conn->rz_max);
    DBG("  rx_offset=%d, rx_size=%d, rx_min=%ld, rx_max=%ld\n",
        conn->rx_offset, conn->rx_size, conn->rx_min, conn->rx_max);
    DBG("  ry_offset=%d, ry_size=%d, ry_min=%ld, ry_max=%ld\n",
        conn->ry_offset, conn->ry_size, conn->ry_min, conn->ry_max);
    DBG("  hat_offset=%d, hat_size=%d, hat_min=%ld, hat_max=%ld\n",
        conn->hat_offset, conn->hat_size, conn->hat_min, conn->hat_max);
    DBG("  button_offsets: ");
    for (int i = 0; i < PAD_MAX_BUTTONS; i++)
    {
        if (conn->button_offsets[i] != 0xFFFF)
            DBG("[%d]=%d ", i, conn->button_offsets[i]);
    }
    DBG("\n");
}

static void pad_distill_descriptor(
    pad_connection_t *conn,
    uint8_t const *desc_data, uint16_t desc_len,
    uint16_t vendor_id, uint16_t product_id)
{
    conn->valid = false;
    pad_parse_descriptor(conn, desc_data, desc_len);

    DBG("Received HID descriptor. vid=0x%04X, pid=0x%04X, len=%d, valid=%d\n",
        vendor_id, product_id, desc_len, conn->valid);

    // Add your gamepad override here.
    pad_remap_8bitdo_m30(conn, vendor_id, product_id);
    pad_remap_playstation_classic(conn, vendor_id, product_id);

    // Sony gamepads use a pre-computed descriptor.
    // Some may report a descriptor, which we discard.
    if (pad_is_sony_ds4(vendor_id, product_id))
    {
        *conn = pad_desc_sony_ds4;
        DBG("Detected Sony DS4 gamepad, using pre-computed descriptor.\n");
    }
    if (pad_is_sony_ds5(vendor_id, product_id))
    {
        *conn = pad_desc_sony_ds5;
        DBG("Detected Sony DS5 gamepad, using pre-computed descriptor.\n");
    }

    if (!conn->valid)
        DBG("HID descriptor not a gamepad.\n");
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

static void pad_parse_report(int player, uint8_t const *data, uint16_t report_len, pad_xram_t *report)
{
    // Default empty gamepad report
    memset(report, 0, sizeof(pad_xram_t));

    // Add feature bits to dpad
    pad_connection_t *gamepad = &pad_connections[player];
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
        uint32_t raw_x = hid_extract_bits(data, report_len, gamepad->x_offset, gamepad->x_size);
        report->lx = hid_scale_analog_signed(raw_x, gamepad->x_size, gamepad->x_min, gamepad->x_max);
    }
    if (gamepad->y_size > 0)
    {
        uint32_t raw_y = hid_extract_bits(data, report_len, gamepad->y_offset, gamepad->y_size);
        report->ly = hid_scale_analog_signed(raw_y, gamepad->y_size, gamepad->y_min, gamepad->y_max);
    }
    if (gamepad->z_size > 0)
    {
        uint32_t raw_z = hid_extract_bits(data, report_len, gamepad->z_offset, gamepad->z_size);
        report->rx = hid_scale_analog_signed(raw_z, gamepad->z_size, gamepad->z_min, gamepad->z_max);
    }
    if (gamepad->rz_size > 0)
    {
        uint32_t raw_rz = hid_extract_bits(data, report_len, gamepad->rz_offset, gamepad->rz_size);
        report->ry = hid_scale_analog_signed(raw_rz, gamepad->rz_size, gamepad->rz_min, gamepad->rz_max);
    }

    // Extract triggers
    if (gamepad->rx_size > 0)
    {
        uint32_t raw_rx = hid_extract_bits(data, report_len, gamepad->rx_offset, gamepad->rx_size);
        report->lt = hid_scale_analog(raw_rx, gamepad->rx_size, gamepad->rx_min, gamepad->rx_max);
    }
    if (gamepad->ry_size > 0)
    {
        uint32_t raw_ry = hid_extract_bits(data, report_len, gamepad->ry_offset, gamepad->ry_size);
        report->rt = hid_scale_analog(raw_ry, gamepad->ry_size, gamepad->ry_min, gamepad->ry_max);
    }

    // Extract buttons using individual bit offsets
    uint32_t buttons = 0;
    for (int i = 0; i < PAD_MAX_BUTTONS; i++)
        if (hid_extract_bits(data, report_len, gamepad->button_offsets[i], 1))
            buttons |= (1UL << i);
    report->button0 = buttons & 0xFF;
    report->button1 = (buttons & 0xFF00) >> 8;

    // Extract D-pad/hat
    if (gamepad->hat_size == 4 && gamepad->hat_max - gamepad->hat_min == 7)
    {
        // Convert HID hat format to individual direction bits
        static const uint8_t hat_to_pad[] = {1, 9, 8, 10, 2, 6, 4, 5};
        uint32_t raw_hat = hid_extract_bits(data, report_len, gamepad->hat_offset, gamepad->hat_size);
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
    pad_xram_t gamepad_report;
    pad_parse_report(player, 0, 0, &gamepad_report); // get blank
    memcpy(&xram[pad_xram + player * (sizeof(pad_xram_t))],
           &gamepad_report, sizeof(pad_xram_t));
}

bool pad_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - (sizeof(pad_xram_t)) * PAD_MAX_PLAYERS)
        return false;
    pad_xram = word;
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
        pad_reset_xram(i);
    return true;
}

bool __in_flash("pad_mount") pad_mount(int slot, uint8_t const *desc_data, uint16_t desc_len,
                                       uint16_t vendor_id, uint16_t product_id)
{
    pad_connection_t *gamepad = NULL;
    int player;
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
    {
        if (!pad_connections[i].valid)
        {
            gamepad = &pad_connections[i];
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

    pad_distill_descriptor(gamepad, desc_data, desc_len, vendor_id, product_id);
    if (gamepad->valid)
    {
        gamepad->slot = slot;
        pad_reset_xram(player);
        return true;
    }
    return false;
}

bool pad_umount(int slot)
{
    // Find the descriptor by dev_addr and slot
    int player = pad_get_player_num(slot);
    if (player < 0)
        return false;
    pad_connection_t *conn = &pad_connections[player];
    conn->valid = false;
    pad_reset_xram(player);
    return true;
}

void pad_report(int slot, uint8_t const *data, uint16_t len)
{
    int player = pad_get_player_num(slot);
    if (player < 0)
        return;
    pad_connection_t *conn = &pad_connections[player];

    const uint8_t *report_data = data;
    uint16_t report_data_len = len;
    if (conn->report_id != 0)
    {
        if (len == 0 || data[0] != conn->report_id)
            return;
        // Skip report ID byte
        report_data = &data[1];
        report_data_len = len - 1;
    }

    // Parse report and send it to xram
    if (pad_xram != 0xFFFF)
    {
        pad_xram_t gamepad_report;
        pad_parse_report(player, report_data, report_data_len, &gamepad_report);
        memcpy(&xram[pad_xram + player * (sizeof(pad_xram_t))],
               &gamepad_report, sizeof(pad_xram_t));
    }
}

// This is for XBox One/Series gamepads which send
// the home button down a different path.
void pad_home_button(int slot, bool pressed)
{
    int player = pad_get_player_num(slot);
    if (player < 0)
        return;
    pad_connection_t *conn = &pad_connections[player];

    // Inject out of band home button into reports
    conn->home_pressed = pressed;

    // Update the home button bit in xram
    if (pad_xram != 0xFFFF)
    {
        uint8_t *button1 = &xram[pad_xram + player * (sizeof(pad_xram_t)) + 3];
        if (pressed)
            *button1 |= (1 << (PAD_HOME_BUTTON - 8));
        else
            *button1 &= ~(1 << (PAD_HOME_BUTTON - 8));
    }
}

// Useful for gamepads that indicate player number.
int pad_get_player_num(int slot)
{
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
        if (pad_connections[i].slot == slot && pad_connections[i].valid)
            return i;
    return -1;
}
