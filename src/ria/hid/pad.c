/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hid/hid.h"
#include "hid/pad.h"
#include "sys/mem.h"
#include <pico.h>
#include <string.h>

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_PAD)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
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
// This is only for the analog to digital conversions so
// it doesn't need to be first-person shooter tight.
#define PAD_DEADZONE 32

// Room for button0 and button1 plus a dpad if needed.
#define PAD_MAX_BUTTONS 20

#define PAD_HOME_BUTTON 12

// LED type for player indicators
enum
{
    PAD_LED_NONE,
    PAD_LED_DS4,
    PAD_LED_DS5,
};

// Gamepad descriptors are normalized to this structure.
typedef struct
{
    bool valid;
    bool sony;         // Indicates gamepad uses sony button labels
    bool home_pressed; // Used to inject the out of band home button on xbox one
    int slot;          // HID protocol drivers use slots assigned in hid.h
    uint8_t led_type;  // PAD_LED_NONE, PAD_LED_DS4, PAD_LED_DS5
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
    if (vendor_id != 0x054C || product_id != 0x05C2)
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
    DBG("8BitDo M30 remap: vid=0x%04X, pid=0x%04X\n", vendor_id, product_id);
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
        case 0x0CDA: // DualShock 4 (Asia region, special edition)
        case 0x0D9A: // DualShock 4 (Japan region, special edition)
        case 0x0E04: // DualShock 4 (rare, but reported)
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

static bool __in_flash("pad_parse") pad_parse_field(const hid_field_t *field, void *context)
{
    pad_connection_t *conn = (pad_connection_t *)context;

    bool get_report_id = false;
    if (field->usage_page == 0x01) // Generic Desktop
    {
        get_report_id = true;
        switch (field->usage)
        {
        case 0x30: // X axis (left stick X)
            conn->x_offset = field->bit_pos;
            conn->x_size = field->size;
            conn->x_min = field->logical_min;
            conn->x_max = field->logical_max;
            conn->x_absolute = !(field->input_flags & 0x04);
            break;
        case 0x31: // Y axis (left stick Y)
            conn->y_offset = field->bit_pos;
            conn->y_size = field->size;
            conn->y_min = field->logical_min;
            conn->y_max = field->logical_max;
            break;
        case 0x32: // Z axis (right stick X)
            conn->z_offset = field->bit_pos;
            conn->z_size = field->size;
            conn->z_min = field->logical_min;
            conn->z_max = field->logical_max;
            break;
        case 0x35: // Rz axis (right stick Y)
            conn->rz_offset = field->bit_pos;
            conn->rz_size = field->size;
            conn->rz_min = field->logical_min;
            conn->rz_max = field->logical_max;
            break;
        case 0x33: // Rx axis (left trigger)
            conn->rx_offset = field->bit_pos;
            conn->rx_size = field->size;
            conn->rx_min = field->logical_min;
            conn->rx_max = field->logical_max;
            break;
        case 0x34: // Ry axis (right trigger)
            conn->ry_offset = field->bit_pos;
            conn->ry_size = field->size;
            conn->ry_min = field->logical_min;
            conn->ry_max = field->logical_max;
            break;
        case 0x39: // Hat switch (D-pad)
            conn->hat_offset = field->bit_pos;
            conn->hat_size = field->size;
            conn->hat_min = field->logical_min;
            conn->hat_max = field->logical_max;
            break;
        }
    }
    else if (field->usage_page == 0x02) // Simulation Controls
    {
        get_report_id = true;
        switch (field->usage)
        {
        case 0xC5: // Brake (left trigger)
            conn->rx_offset = field->bit_pos;
            conn->rx_size = field->size;
            conn->rx_min = field->logical_min;
            conn->rx_max = field->logical_max;
            break;
        case 0xC4: // Accelerator (right trigger)
            conn->ry_offset = field->bit_pos;
            conn->ry_size = field->size;
            conn->ry_min = field->logical_min;
            conn->ry_max = field->logical_max;
            break;
        }
    }
    else if (field->usage_page == 0x09) // Button page
    {
        get_report_id = true;
        uint8_t button_index = field->usage - 1;
        if (button_index < PAD_MAX_BUTTONS)
            conn->button_offsets[button_index] = field->bit_pos;
    }
    if (get_report_id && conn->report_id == 0 && field->report_id != 0xFFFF)
        conn->report_id = field->report_id;

    return true;
}

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

    hid_descriptor_parse(desc_data, desc_len, pad_parse_field, conn);

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

    // Sony gamepads use a pre-computed descriptor.
    // Some may report a descriptor, which we discard.
    if (pad_is_sony_ds4(vendor_id, product_id))
    {
        *conn = pad_desc_sony_ds4;
        conn->led_type = PAD_LED_DS4;
        DBG("Detected Sony DS4 gamepad, using pre-computed descriptor.\n");
    }
    else if (pad_is_sony_ds5(vendor_id, product_id))
    {
        *conn = pad_desc_sony_ds5;
        conn->led_type = PAD_LED_DS5;
        DBG("Detected Sony DS5 gamepad, using pre-computed descriptor.\n");
    }
    else
    {
        pad_parse_descriptor(conn, desc_data, desc_len);

        DBG("Received HID descriptor. vid=0x%04X, pid=0x%04X, len=%d, valid=%d\n",
            vendor_id, product_id, desc_len, conn->valid);

        // Add your gamepad override here.
        pad_remap_8bitdo_m30(conn, vendor_id, product_id);
        pad_remap_playstation_classic(conn, vendor_id, product_id);
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
    pad_connection_t *conn = &pad_connections[player];
    if (conn->valid)
    {
        report->dpad |= 0x80;
        if (conn->sony)
            report->dpad |= 0x40;
    }

    // A blank report was requested
    if (report_len == 0)
        return;

    // Extract analog sticks
    if (conn->x_size > 0)
    {
        uint32_t raw_x = hid_extract_bits(data, report_len, conn->x_offset, conn->x_size);
        report->lx = hid_scale_analog_signed(raw_x, conn->x_size, conn->x_min, conn->x_max);
    }
    if (conn->y_size > 0)
    {
        uint32_t raw_y = hid_extract_bits(data, report_len, conn->y_offset, conn->y_size);
        report->ly = hid_scale_analog_signed(raw_y, conn->y_size, conn->y_min, conn->y_max);
    }
    if (conn->z_size > 0)
    {
        uint32_t raw_z = hid_extract_bits(data, report_len, conn->z_offset, conn->z_size);
        report->rx = hid_scale_analog_signed(raw_z, conn->z_size, conn->z_min, conn->z_max);
    }
    if (conn->rz_size > 0)
    {
        uint32_t raw_rz = hid_extract_bits(data, report_len, conn->rz_offset, conn->rz_size);
        report->ry = hid_scale_analog_signed(raw_rz, conn->rz_size, conn->rz_min, conn->rz_max);
    }

    // Extract triggers
    if (conn->rx_size > 0)
    {
        uint32_t raw_rx = hid_extract_bits(data, report_len, conn->rx_offset, conn->rx_size);
        report->lt = hid_scale_analog(raw_rx, conn->rx_size, conn->rx_min, conn->rx_max);
    }
    if (conn->ry_size > 0)
    {
        uint32_t raw_ry = hid_extract_bits(data, report_len, conn->ry_offset, conn->ry_size);
        report->rt = hid_scale_analog(raw_ry, conn->ry_size, conn->ry_min, conn->ry_max);
    }

    // Extract buttons using individual bit offsets
    uint32_t buttons = 0;
    for (int i = 0; i < PAD_MAX_BUTTONS; i++)
    {
        if (conn->button_offsets[i] == 0xFFFF)
            continue;
        if (hid_extract_bits(data, report_len, conn->button_offsets[i], 1))
            buttons |= (1UL << i);
    }
    report->button0 = buttons & 0xFF;
    report->button1 = (buttons & 0xFF00) >> 8;

    // Extract D-pad/hat
    if (conn->hat_size == 4 && conn->hat_max - conn->hat_min == 7)
    {
        // Convert HID hat format to individual direction bits
        static const uint8_t hat_to_pad[] = {1, 9, 8, 10, 2, 6, 4, 5};
        uint32_t raw_hat = hid_extract_bits(data, report_len, conn->hat_offset, conn->hat_size);
        unsigned index = raw_hat - conn->hat_min;
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
    if (conn->home_pressed)
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
    pad_connection_t *conn = NULL;
    int player;
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
    {
        if (!pad_connections[i].valid)
        {
            conn = &pad_connections[i];
            player = i;
            break;
        }
    }
    if (!conn)
    {
        DBG("pad_mount: No available descriptor slots, max players reached\n");
        return false;
    }
    DBG("pad_mount: mounting player %d\n", player);

    pad_distill_descriptor(conn, desc_data, desc_len, vendor_id, product_id);
    if (conn->valid)
    {
        conn->slot = slot;
        pad_reset_xram(player);
        return true;
    }
    return false;
}

// Useful for gamepads that indicate player number.
int pad_get_player_num(int slot)
{
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
        if (pad_connections[i].slot == slot && pad_connections[i].valid)
            return i;
    return -1;
}

bool pad_umount(int slot)
{
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
        uint8_t *button1 = &xram[pad_xram + player * sizeof(pad_xram_t) + offsetof(pad_xram_t, button1)];
        if (pressed)
            *button1 |= (1 << (PAD_HOME_BUTTON - 8));
        else
            *button1 &= ~(1 << (PAD_HOME_BUTTON - 8));
    }
}

// Build LED output report for player indicator on Sony controllers.
// Writes into buf which must be PAD_LED_REPORT_MAX bytes.
// Sets report_id and report_len. Returns true if a LED report was written.
_Static_assert(PAD_LED_REPORT_MAX >= 47, "PAD_LED_REPORT_MAX too small for DS5");
_Static_assert(PAD_LED_REPORT_MAX >= 31, "PAD_LED_REPORT_MAX too small for DS4");
bool pad_build_led_report(int slot, uint8_t buf[PAD_LED_REPORT_MAX],
                          uint8_t *report_id, uint16_t *report_len)
{
    int player = pad_get_player_num(slot);
    if (player < 0)
        return false;

    pad_connection_t *conn = &pad_connections[player];

    // Player indicator colors: Blue, Red, Green, Pink
    static const uint8_t player_colors[][3] = {
        {0x00, 0x00, 0x40},
        {0x40, 0x00, 0x00},
        {0x00, 0x40, 0x00},
        {0x20, 0x00, 0x20},
    };

    switch (conn->led_type)
    {
    case PAD_LED_DS5:
    {
        // DualSense: player indicator LEDs + lightbar color
        // Player LED patterns: P1=center, P2=inner pair, P3=three, P4=four
        static const uint8_t ds5_player_leds[] = {0x04, 0x0A, 0x15, 0x1B};
        memset(buf, 0, 47);
        buf[1] = 0x14;                      // valid_flag1: player LEDs (0x10) + lightbar (0x04)
        buf[38] = 0x02;                     // valid_flag2: lightbar setup
        buf[43] = ds5_player_leds[player];  // player LED pattern
        buf[44] = player_colors[player][0]; // R
        buf[45] = player_colors[player][1]; // G
        buf[46] = player_colors[player][2]; // B
        *report_id = 2;
        *report_len = 47;
        return true;
    }
    case PAD_LED_DS4:
    {
        // DualShock 4: lightbar color for player indication
        memset(buf, 0, 31);
        buf[0] = 0xFF;                     // enable all features
        buf[5] = player_colors[player][0]; // R
        buf[6] = player_colors[player][1]; // G
        buf[7] = player_colors[player][2]; // B
        *report_id = 5;
        *report_len = 31;
        return true;
    }
    default:
        return false;
    }
}
