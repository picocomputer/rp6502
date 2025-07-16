/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb_config.h"
#include "usb/pad.h"
#include "usb/des.h"
#include "sys/mem.h"
#include <stdlib.h>
#include <string.h>

#define DEBUG_RIA_USB_PAD

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_PAD)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...) \
    {            \
    }
#endif

// hat values: 0-7 clockwise, 0 = north, 8 = no press
// Feature bit 0x80 is on when valid controller connected
// Feature bit 0x40 is on when Sony-style controller detected
typedef struct TU_ATTR_PACKED
{
    uint8_t hat;     // hat (0x0F) and feature (0xF0) bits
    uint8_t sticks;  // left (0x0F) and right (0xF0) sticks as hat values
    uint8_t button0; // buttons
    uint8_t button1; // buttons
    uint8_t x;       // left analog-stick
    uint8_t y;       // left analog-stick
    uint8_t z;       // right analog-stick
    uint8_t rz;      // right analog-stick
    uint8_t rx;      // analog left trigger
    uint8_t ry;      // analog right trigger
} pad_gamepad_report_t;

// Deadzone should be generous enough for moderately worn sticks.
// Apps should use analog values if they want to tighten it up.
#define PAD_DEADZONE 32
#define PAD_PLAYER_LEN 4

static int8_t pad_player_idx[PAD_PLAYER_LEN];
static uint16_t pad_xram;
static pad_descriptor_t pad_descriptors[CFG_TUH_HID];

static uint32_t pad_extract_bits(uint8_t const *report, uint16_t report_len, uint8_t bit_offset, uint8_t bit_size)
{
    if (bit_size == 0 || bit_size > 32)
        return 0;

    uint8_t byte_offset = bit_offset / 8;
    uint8_t bit_shift = bit_offset % 8;
    uint8_t bytes_needed = (bit_offset + bit_size + 7) / 8;
    if (bytes_needed > report_len)
        return 0;

    // Extract value across multiple bytes if needed
    uint32_t value = 0;
    for (uint8_t i = 0; i < (bit_size + 7) / 8 && i < 4 && (byte_offset + i) < report_len; i++)
        value |= ((uint32_t)report[byte_offset + i]) << (i * 8);

    // Shift and mask to get the desired bits
    value >>= bit_shift;
    if (bit_size < 32)
        value &= (1UL << bit_size) - 1;

    return value;
}

static uint8_t pad_encode_hat(uint8_t x_raw, uint8_t y_raw)
{
    // Calculate offset from center (127)
    int8_t x = x_raw - 127;
    int8_t y = 127 - y_raw; // Invert Y so positive is up (north)

    // Check deadzone
    if ((x > -PAD_DEADZONE && x < PAD_DEADZONE) && (y > -PAD_DEADZONE && y < PAD_DEADZONE))
        return 8; // No direction

    // Determine direction based on octant
    // First check if we're in a primarily cardinal direction
    if (y > abs(x) * 2)
        return 0; // North
    if (x > abs(y) * 2)
        return 2; // East
    if (y < -abs(x) * 2)
        return 4; // SouthH
    if (x < -abs(y) * 2)
        return 6; // West

    // If not cardinal, then we're in a diagonal
    if (y > 0 && x > 0)
        return 1; // North-East
    if (y < 0 && x > 0)
        return 3; // South-East
    if (y < 0 && x < 0)
        return 5;                  // South-West
    /* y > 0 && x < 0 */ return 7; // North-West
}

static void pad_parse_report_to_gamepad(uint8_t idx, uint8_t const *report, uint16_t report_len, pad_gamepad_report_t *gamepad_report)
{

    // Default empty gamepad report
    memset(gamepad_report, 0, sizeof(pad_gamepad_report_t));
    gamepad_report->hat = 0x08;
    gamepad_report->sticks = 0x88;
    gamepad_report->x = 127;
    gamepad_report->y = 127;
    gamepad_report->z = 127;
    gamepad_report->rz = 127;
    if (report_len == 0)
        return;

    pad_descriptor_t *desc = &pad_descriptors[idx];

    // Check if this is an Xbox One controller (detect by report structure)
    bool is_xbox_one = (desc->hat_size == 0 && desc->x_size == 16 && desc->y_size == 16 &&
                       desc->z_size == 16 && desc->rz_size == 16 && desc->rx_size == 16);

    // Extract analog sticks
    if (desc->x_size > 0)
    {
        uint32_t raw_x = pad_extract_bits(report, report_len, desc->x_offset, desc->x_size);
        if (is_xbox_one && desc->x_size == 16)
        {
            // Xbox One uses 16-bit signed values, convert to 8-bit range
            int16_t signed_x = (int16_t)raw_x;
            gamepad_report->x = (uint8_t)((signed_x + 32768) >> 8);
        }
        else
        {
            gamepad_report->x = (uint8_t)(raw_x >> (desc->x_size > 8 ? desc->x_size - 8 : 0));
        }
    }
    if (desc->y_size > 0)
    {
        uint32_t raw_y = pad_extract_bits(report, report_len, desc->y_offset, desc->y_size);
        if (is_xbox_one && desc->y_size == 16)
        {
            // Xbox One uses 16-bit signed values, convert to 8-bit range
            int16_t signed_y = (int16_t)raw_y;
            gamepad_report->y = (uint8_t)((signed_y + 32768) >> 8);
        }
        else
        {
            gamepad_report->y = (uint8_t)(raw_y >> (desc->y_size > 8 ? desc->y_size - 8 : 0));
        }
    }
    if (desc->z_size > 0)
    {
        uint32_t raw_z = pad_extract_bits(report, report_len, desc->z_offset, desc->z_size);
        if (is_xbox_one && desc->z_size == 16)
        {
            // Xbox One uses 16-bit signed values, convert to 8-bit range
            int16_t signed_z = (int16_t)raw_z;
            gamepad_report->z = (uint8_t)((signed_z + 32768) >> 8);
        }
        else
        {
            gamepad_report->z = (uint8_t)(raw_z >> (desc->z_size > 8 ? desc->z_size - 8 : 0));
        }
    }
    if (desc->rz_size > 0)
    {
        uint32_t raw_rz = pad_extract_bits(report, report_len, desc->rz_offset, desc->rz_size);
        if (is_xbox_one && desc->rz_size == 16)
        {
            // Xbox One uses 16-bit signed values, convert to 8-bit range
            int16_t signed_rz = (int16_t)raw_rz;
            gamepad_report->rz = (uint8_t)((signed_rz + 32768) >> 8);
        }
        else
        {
            gamepad_report->rz = (uint8_t)(raw_rz >> (desc->rz_size > 8 ? desc->rz_size - 8 : 0));
        }
    }

    // Extract triggers
    if (desc->rx_size > 0)
    {
        uint32_t raw_rx = pad_extract_bits(report, report_len, desc->rx_offset, desc->rx_size);
        if (is_xbox_one && desc->rx_size == 16)
        {
            // Xbox One uses 16-bit trigger values (10-bit actual), scale to 8-bit
            gamepad_report->rx = (uint8_t)(raw_rx >> 2); // Scale down from 10-bit to 8-bit
        }
        else
        {
            gamepad_report->rx = (uint8_t)(raw_rx >> (desc->rx_size > 8 ? desc->rx_size - 8 : 0));
        }
    }
    if (desc->ry_size > 0)
    {
        uint32_t raw_ry = pad_extract_bits(report, report_len, desc->ry_offset, desc->ry_size);
        if (is_xbox_one && desc->ry_size == 16)
        {
            // Xbox One uses 16-bit trigger values (10-bit actual), scale to 8-bit
            gamepad_report->ry = (uint8_t)(raw_ry >> 2); // Scale down from 10-bit to 8-bit
        }
        else
        {
            gamepad_report->ry = (uint8_t)(raw_ry >> (desc->ry_size > 8 ? desc->ry_size - 8 : 0));
        }
    }

    // Extract D-pad/hat
    if (is_xbox_one)
    {
        // Xbox One sends D-pad as individual button bits in byte 1
        uint8_t dpad_byte = (report_len > 1) ? report[1] : 0;
        uint8_t hat_value = 8; // Default to no press

        bool up = (dpad_byte & (1 << 0)) != 0;
        bool down = (dpad_byte & (1 << 1)) != 0;
        bool left = (dpad_byte & (1 << 2)) != 0;
        bool right = (dpad_byte & (1 << 3)) != 0;

        // Convert individual dpad buttons to hat values (0-7 clockwise from north, 8=no press)
        if (up && !down && !left && !right) hat_value = 0;      // North
        else if (up && !down && !left && right) hat_value = 1;  // North-East
        else if (!up && !down && !left && right) hat_value = 2; // East
        else if (!up && down && !left && right) hat_value = 3;  // South-East
        else if (!up && down && !left && !right) hat_value = 4; // South
        else if (!up && down && left && !right) hat_value = 5;  // South-West
        else if (!up && !down && left && !right) hat_value = 6; // West
        else if (up && !down && left && !right) hat_value = 7;  // North-West

        gamepad_report->hat = hat_value;
    }
    else if (desc->hat_size > 0)
    {
        uint32_t raw_hat = pad_extract_bits(report, report_len, desc->hat_offset, desc->hat_size);
        if (raw_hat > 8)
            raw_hat = 8;
        gamepad_report->hat = (uint8_t)raw_hat;
    }

    // Add feature bits to hat
    if (desc->valid)
        gamepad_report->hat |= 0x80;
    if (desc->sony)
        gamepad_report->hat |= 0x40;

    // Extract buttons using individual bit offsets
    uint32_t buttons = 0;
    for (int i = 0; i < PAD_MAX_BUTTONS && desc->button_offsets[i] != 0xFFFF; i++)
        if (pad_extract_bits(report, report_len, desc->button_offsets[i], 1))
            buttons |= (1UL << i);
    gamepad_report->button0 = buttons & 0xFF;
    gamepad_report->button1 = (buttons & 0xFF00) >> 8;

    // Generate hat values for sticks
    uint8_t hat_l = pad_encode_hat(gamepad_report->x, gamepad_report->y);
    uint8_t hat_r = pad_encode_hat(gamepad_report->z, gamepad_report->rz);
    gamepad_report->sticks = hat_l | (hat_r << 4);

    // If L2/R2 buttons pressed without any analog movement
    if ((buttons & (1 << 6)) && (gamepad_report->rx == 0))
        gamepad_report->rx = 255;
    if ((buttons & (1 << 7)) && (gamepad_report->ry == 0))
        gamepad_report->ry = 255;

    // If L2/R2 analog movement ensure button press
    if (gamepad_report->rx > PAD_DEADZONE)
        gamepad_report->button0 |= (1 << 6); // L2
    if (gamepad_report->ry > PAD_DEADZONE)
        gamepad_report->button0 |= (1 << 7); // R2
}

void pad_init(void)
{
    pad_stop();
    for (int i = 0; i < PAD_PLAYER_LEN; i++)
        pad_player_idx[i] = -1;
}

void pad_stop(void)
{
    pad_xram = 0xFFFF;
}

static void pad_reset_xram(uint8_t player)
{
    if (pad_xram == 0xFFFF)
        return;
    pad_gamepad_report_t gamepad_report;
    pad_parse_report_to_gamepad(0, 0, 0, &gamepad_report); // get blank
    memcpy(&xram[pad_xram + player * sizeof(pad_gamepad_report_t)],
           &gamepad_report,
           sizeof(pad_gamepad_report_t));
}

bool pad_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - sizeof(pad_gamepad_report_t) * PAD_PLAYER_LEN)
        return false;
    pad_xram = word;
    for (int i = 0; i < PAD_PLAYER_LEN; i++)
        pad_reset_xram(i);
    return true;
}

void pad_mount(uint8_t idx, uint8_t const *desc_report, uint16_t desc_len,
               uint16_t vendor_id, uint16_t product_id)
{
    pad_descriptor_t *pad_desc = &pad_descriptors[idx];
    des_report_descriptor(pad_desc, desc_report, desc_len,
                          vendor_id, product_id);

    // Try to assign to an available player slot
    if (pad_desc->valid)
    {
        for (int i = 0; i < PAD_PLAYER_LEN; i++)
        {
            if (pad_player_idx[i] == -1)
            {
                pad_player_idx[i] = idx;
                pad_reset_xram(i);
                break;
            }
        }
    }
}

void pad_umount(uint8_t idx)
{
    pad_descriptors[idx].valid = false;
    for (int i = 0; i < PAD_PLAYER_LEN; i++)
        if (pad_player_idx[i] == idx)
        {
            pad_player_idx[i] = -1;
            pad_reset_xram(i);
        }
}

void pad_report(uint8_t idx, uint8_t const *report, uint16_t len)
{
    pad_descriptor_t *desc = &pad_descriptors[idx];

    if (!desc->valid)
        return;

    // Skip report ID check if no report ID is expected, or validate if one is expected
    const uint8_t *report_data = report;
    uint16_t report_data_len = len;
    if (desc->report_id != 0)
    {
        if (len == 0 || report[0] != desc->report_id)
        {
            DBG("pad_report: Report ID mismatch. Expected %d, got %d\n", desc->report_id, len > 0 ? report[0] : -1);
            return;
        }
        report_data = &report[1]; // Skip report ID byte
        report_data_len = len - 1;
    }

    int player = -1;

    // Check if this device is assigned to a player
    for (int i = 0; i < PAD_PLAYER_LEN; i++)
    {
        if (pad_player_idx[i] == idx)
        {
            player = i;
            break;
        }
    }

    if (player != -1)
    {
        if (pad_xram != 0xFFFF)
        {
            pad_gamepad_report_t gamepad_report;
            pad_parse_report_to_gamepad(idx, report_data, report_data_len, &gamepad_report);
            memcpy(&xram[pad_xram + player * sizeof(pad_gamepad_report_t)], &gamepad_report, sizeof(pad_gamepad_report_t));
        }
    }
}

bool pad_is_valid(uint8_t idx)
{
    return pad_descriptors[idx].valid;
}

void pad_mount_xbox_controller(uint8_t idx, uint16_t vendor_id, uint16_t product_id)
{
    if (idx >= CFG_TUH_HID)
        return;

    pad_descriptor_t *desc = &pad_descriptors[idx];

    // Create a fake descriptor for Xbox One controller
    // This sets up the parsing parameters for Xbox One GIP reports
    memset(desc, 0, sizeof(pad_descriptor_t));

    desc->valid = true;
    desc->sony = false;
    desc->report_id = 0; // Xbox One doesn't use report IDs in vendor mode

    // Xbox One GIP report structure (after 4-byte header):
    // Byte 0: D-pad buttons (individual bits, not hat)
    // Byte 1: Menu/Guide buttons
    // Byte 2: Face buttons (A, B, X, Y) + shoulder buttons
    // Byte 3: Stick buttons (L3, R3)
    // Bytes 4-5: Left trigger (16-bit)
    // Bytes 6-7: Right trigger (16-bit)
    // Bytes 8-9: Left stick X (16-bit signed)
    // Bytes 10-11: Left stick Y (16-bit signed)
    // Bytes 12-13: Right stick X (16-bit signed)
    // Bytes 14-15: Right stick Y (16-bit signed)

    // Set up analog stick parsing (16-bit signed values)
    desc->x_offset = 8 * 8;   // Byte 8, bit 0
    desc->x_size = 16;
    desc->y_offset = 10 * 8;  // Byte 10, bit 0
    desc->y_size = 16;
    desc->z_offset = 12 * 8;  // Byte 12, bit 0
    desc->z_size = 16;
    desc->rz_offset = 14 * 8; // Byte 14, bit 0
    desc->rz_size = 16;

    // Set up trigger parsing (16-bit values, 10-bit precision)
    desc->rx_offset = 4 * 8;  // Byte 4, bit 0 (left trigger)
    desc->rx_size = 16;
    desc->ry_offset = 6 * 8;  // Byte 6, bit 0 (right trigger)
    desc->ry_size = 16;

    // Xbox One sends D-pad as individual bits, not as hat
    desc->hat_offset = 0;
    desc->hat_size = 0;

    // Set up button mappings for Xbox One controller
    // Face buttons (A, B, X, Y) are in byte 2, bits 0-3
    desc->button_offsets[0] = 2 * 8 + 0; // A button
    desc->button_offsets[1] = 2 * 8 + 1; // B button
    desc->button_offsets[2] = 2 * 8 + 2; // X button
    desc->button_offsets[3] = 2 * 8 + 3; // Y button

    // Shoulder buttons are in byte 2, bits 4-5
    desc->button_offsets[4] = 2 * 8 + 4; // LB (left bumper)
    desc->button_offsets[5] = 2 * 8 + 5; // RB (right bumper)

    // Trigger buttons will be handled as analog in parsing code
    desc->button_offsets[6] = 0xFFFF; // LT (will be set based on analog value)
    desc->button_offsets[7] = 0xFFFF; // RT (will be set based on analog value)

    // Start/Select equivalents are in byte 1
    desc->button_offsets[8] = 1 * 8 + 2; // Menu button (Start)
    desc->button_offsets[9] = 1 * 8 + 3; // View button (Select)

    // Stick clicks are in byte 3
    desc->button_offsets[10] = 3 * 8 + 0; // L3 (left stick click)
    desc->button_offsets[11] = 3 * 8 + 1; // R3 (right stick click)

    // Guide button is in byte 1, bit 0
    desc->button_offsets[12] = 1 * 8 + 0; // Guide/Xbox button

    // Mark remaining buttons as unused
    for (int i = 13; i < PAD_MAX_BUTTONS; i++) {
        desc->button_offsets[i] = 0xFFFF;
    }

    DBG("pad_mount_xbox_controller: Xbox controller mounted at idx %d\n", idx);

    // Try to assign to an available player slot
    for (int i = 0; i < PAD_PLAYER_LEN; i++) {
        if (pad_player_idx[i] == -1) {
            pad_player_idx[i] = idx;
            pad_reset_xram(i);
            DBG("pad_mount_xbox_controller: Assigned to player %d\n", i);
            break;
        }
    }
}

void pad_umount_xbox_controller(uint8_t idx)
{
    DBG("pad_umount_xbox_controller: Unmounting Xbox controller at idx %d\n", idx);
    pad_umount(idx);
}

void pad_report_xbox_controller(uint8_t idx, uint8_t const *report, uint16_t len)
{
    DBG("pad_report_xbox_controller: Received report from idx %d, len %d\n", idx, len);
    pad_report(idx, report, len);
}
