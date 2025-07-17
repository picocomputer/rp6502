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
    int8_t x;        // left analog-stick
    int8_t y;        // left analog-stick
    int8_t z;        // right analog-stick
    int8_t rz;       // right analog-stick
    uint8_t rx;      // analog left trigger
    uint8_t ry;      // analog right trigger
} pad_gamepad_report_t;

// Deadzone should be generous enough for moderately worn sticks.
// Apps should use analog values if they want to tighten it up.
#define PAD_DEADZONE 32

static uint16_t pad_xram;
static pad_descriptor_t pad_players[PAD_PLAYER_LEN];

// Forward declarations
static uint8_t pad_scale_analog(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max);
static int8_t pad_scale_analog_signed(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max);
static uint8_t pad_encode_hat(int8_t x_raw, int8_t y_raw);

static uint32_t pad_extract_bits(uint8_t const *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size)
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

static uint8_t pad_scale_analog(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max)
{
    // Handle signed values by treating them as such
    int32_t signed_value;
    if (bit_size <= 8)
    {
        signed_value = (int8_t)raw_value;
    }
    else if (bit_size <= 16)
    {
        signed_value = (int16_t)raw_value;
    }
    else
    {
        signed_value = (int32_t)raw_value;
    }

    // If the value appears to be outside the expected range, extend sign
    if (logical_min < 0 && bit_size < 32)
    {
        // Sign extend for negative logical minimum
        uint32_t sign_bit = 1UL << (bit_size - 1);
        if (raw_value & sign_bit)
        {
            signed_value = (int32_t)(raw_value | (~((1UL << bit_size) - 1)));
        }
        else
        {
            signed_value = (int32_t)raw_value;
        }
    }

    // Clamp to logical range
    if (signed_value < logical_min)
        signed_value = logical_min;
    if (signed_value > logical_max)
        signed_value = logical_max;

    // Scale to 0-255 range
    int32_t range = logical_max - logical_min;
    if (range == 0)
        return 127; // Avoid division by zero

    int32_t normalized = signed_value - logical_min;
    return (uint8_t)((normalized * 255) / range);
}

static int8_t pad_scale_analog_signed(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max)
{
    // Handle signed values by treating them as such
    int32_t signed_value;
    if (bit_size <= 8)
    {
        signed_value = (int8_t)raw_value;
    }
    else if (bit_size <= 16)
    {
        signed_value = (int16_t)raw_value;
    }
    else
    {
        signed_value = (int32_t)raw_value;
    }

    // If the value appears to be outside the expected range, extend sign
    if (logical_min < 0 && bit_size < 32)
    {
        // Sign extend for negative logical minimum
        uint32_t sign_bit = 1UL << (bit_size - 1);
        if (raw_value & sign_bit)
        {
            signed_value = (int32_t)(raw_value | (~((1UL << bit_size) - 1)));
        }
        else
        {
            signed_value = (int32_t)raw_value;
        }
    }
    else if (logical_min >= 0)
    {
        // For unsigned logical ranges (like 0-255), treat raw_value as unsigned
        signed_value = (int32_t)raw_value;
    }

    // Clamp to logical range
    if (signed_value < logical_min)
        signed_value = logical_min;
    if (signed_value > logical_max)
        signed_value = logical_max;

    // Scale to -128 to 127 range
    int32_t range = logical_max - logical_min;
    if (range == 0)
        return 0; // Avoid division by zero

    // For unsigned logical ranges, map the center to 0
    if (logical_min >= 0)
    {
        // Map logical range to -128 to 127, with center at 0
        int32_t center = logical_min + range / 2;
        int32_t offset = signed_value - center;
        // Scale the offset to fit in -128 to 127 range
        return (int8_t)((offset * 255) / range);
    }
    else
    {
        // For signed logical ranges, use standard scaling
        int32_t normalized = signed_value - logical_min;
        return (int8_t)((normalized * 255) / range - 128);
    }
}

static uint8_t pad_encode_hat(int8_t x_raw, int8_t y_raw)
{
    // x_raw and y_raw are already centered at 0
    int8_t x = x_raw;
    int8_t y = -y_raw; // Invert Y so positive is up (north)

    // Check deadzone
    if ((x > -PAD_DEADZONE && x < PAD_DEADZONE) && (y > -PAD_DEADZONE && y < PAD_DEADZONE))
        return 0; // No direction

    // Convert to direction bits (same format as D-pad processing)
    // Bit 0 = North (1), Bit 1 = South (2), Bit 2 = West (4), Bit 3 = East (8)
    // Determine direction based on octant
    // First check if we're in a primarily cardinal direction
    if (y > abs(x) * 2)
        return 1; // North
    if (x > abs(y) * 2)
        return 8; // East
    if (y < -abs(x) * 2)
        return 2; // South
    if (x < -abs(y) * 2)
        return 4; // West

    // If not cardinal, then we're in a diagonal
    if (y > 0 && x > 0)
        return 9; // North-East (1 + 8)
    if (y < 0 && x > 0)
        return 10; // South-East (2 + 8)
    if (y < 0 && x < 0)
        return 6; // South-West (2 + 4)
    /* y > 0 && x < 0 */
    return 5; // North-West (1 + 4)
}

int pad_find_player_by_idx(uint8_t idx)
{
    for (int i = 0; i < PAD_PLAYER_LEN; i++)
        if (pad_players[i].idx == idx && pad_players[i].valid)
            return i;
    return -1;
}

static void pad_parse_report_to_gamepad(int player, uint8_t const *report, uint16_t report_len, pad_gamepad_report_t *gamepad_report)
{

    // Default empty gamepad report
    memset(gamepad_report, 0, sizeof(pad_gamepad_report_t));
    gamepad_report->hat = 0x08;
    gamepad_report->sticks = 0x88;
    gamepad_report->x = 0;
    gamepad_report->y = 0;
    gamepad_report->z = 0;
    gamepad_report->rz = 0;
    if (report_len == 0)
        return;

    pad_descriptor_t *desc = &pad_players[player];

    // Extract analog sticks
    if (desc->x_size > 0)
    {
        uint32_t raw_x = pad_extract_bits(report, report_len, desc->x_offset, desc->x_size);
        gamepad_report->x = pad_scale_analog_signed(raw_x, desc->x_size, desc->x_logical_min, desc->x_logical_max);
    }
    if (desc->y_size > 0)
    {
        uint32_t raw_y = pad_extract_bits(report, report_len, desc->y_offset, desc->y_size);
        gamepad_report->y = pad_scale_analog_signed(raw_y, desc->y_size, desc->y_logical_min, desc->y_logical_max);
    }
    if (desc->z_size > 0)
    {
        uint32_t raw_z = pad_extract_bits(report, report_len, desc->z_offset, desc->z_size);
        gamepad_report->z = pad_scale_analog_signed(raw_z, desc->z_size, desc->z_logical_min, desc->z_logical_max);
    }
    if (desc->rz_size > 0)
    {
        uint32_t raw_rz = pad_extract_bits(report, report_len, desc->rz_offset, desc->rz_size);
        gamepad_report->rz = pad_scale_analog_signed(raw_rz, desc->rz_size, desc->rz_logical_min, desc->rz_logical_max);
    }

    // Extract triggers
    if (desc->rx_size > 0)
    {
        uint32_t raw_rx = pad_extract_bits(report, report_len, desc->rx_offset, desc->rx_size);
        gamepad_report->rx = pad_scale_analog(raw_rx, desc->rx_size, desc->rx_logical_min, desc->rx_logical_max);
    }
    if (desc->ry_size > 0)
    {
        uint32_t raw_ry = pad_extract_bits(report, report_len, desc->ry_offset, desc->ry_size);
        gamepad_report->ry = pad_scale_analog(raw_ry, desc->ry_size, desc->ry_logical_min, desc->ry_logical_max);
    }

    // Extract buttons using individual bit offsets
    uint32_t buttons = 0;
    for (int i = 0; i < PAD_MAX_BUTTONS; i++)
        if (pad_extract_bits(report, report_len, desc->button_offsets[i], 1))
            buttons |= (1UL << i);
    gamepad_report->button0 = buttons & 0xFF;
    gamepad_report->button1 = (buttons & 0xFF00) >> 8;

    // Extract D-pad/hat
    if (desc->hat_size > 0)
    {
        // Standard HID hat switch - convert to individual button format
        uint32_t raw_hat = pad_extract_bits(report, report_len, desc->hat_offset, desc->hat_size);
        // Convert HID hat format (0-7 clockwise, 8=none) to individual direction bits
        switch (raw_hat)
        {
        case 0: // North
            gamepad_report->hat = 1;
            break;
        case 1: // North-East
            gamepad_report->hat = 9;
            break;
        case 2: // East
            gamepad_report->hat = 8;
            break;
        case 3: // South-East
            gamepad_report->hat = 10;
            break;
        case 4: // South
            gamepad_report->hat = 2;
            break;
        case 5: // South-West
            gamepad_report->hat = 6;
            break;
        case 6: // West
            gamepad_report->hat = 4;
            break;
        case 7: // North-West
            gamepad_report->hat = 5;
            break;
        default: // No direction (8) or invalid
            gamepad_report->hat = 0;
            break;
        }
    }
    else
    {
        gamepad_report->hat = (buttons & 0xF0000) >> 16;
    }

    // Add feature bits to hat
    if (desc->valid)
        gamepad_report->hat |= 0x80;
    if (desc->sony)
        gamepad_report->hat |= 0x40;

    // Generate hat values for sticks
    uint8_t hat_l = pad_encode_hat(gamepad_report->x, gamepad_report->y);
    uint8_t hat_r = pad_encode_hat(gamepad_report->z, gamepad_report->rz);
    gamepad_report->sticks = hat_l | (hat_r << 4);

    // If L2/R2 buttons pressed without any analog movement
    if ((buttons & (1 << 8)) && (gamepad_report->rx == 0))
        gamepad_report->rx = 255;
    if ((buttons & (1 << 9)) && (gamepad_report->ry == 0))
        gamepad_report->ry = 255;

    // If L2/R2 analog movement ensure button press
    if (gamepad_report->rx > PAD_DEADZONE)
        gamepad_report->button1 |= (1 << 0); // L2 (bit 8 -> button1 bit 0)
    if (gamepad_report->ry > PAD_DEADZONE)
        gamepad_report->button1 |= (1 << 1); // R2 (bit 9 -> button1 bit 1)
}

void pad_init(void)
{
    pad_stop();
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
    memcpy(&xram[pad_xram + player * (sizeof(pad_gamepad_report_t))],
           &gamepad_report, sizeof(pad_gamepad_report_t));
}

bool pad_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - (sizeof(pad_gamepad_report_t)) * PAD_PLAYER_LEN)
        return false;
    pad_xram = word;
    for (int i = 0; i < PAD_PLAYER_LEN; i++)
        pad_reset_xram(i);
    return true;
}

void pad_mount(uint8_t idx, uint8_t const *desc_report, uint16_t desc_len,
               uint8_t dev_addr, uint16_t vendor_id, uint16_t product_id)
{
    // Find an available descriptor slot
    pad_descriptor_t *pad_desc = NULL;
    int player;
    for (int i = 0; i < PAD_PLAYER_LEN; i++)
    {
        if (!pad_players[i].valid)
        {
            pad_desc = &pad_players[i];
            player = i;
            break;
        }
    }

    if (!pad_desc)
    {
        DBG("pad_mount: No available descriptor slots\n");
        return;
    }

    des_report_descriptor(pad_desc, desc_report, desc_len,
                          dev_addr, vendor_id, product_id);

    // Try to assign to an available player slot
    if (pad_desc->valid)
    {
        pad_desc->idx = idx;    // Store the interface index
        pad_reset_xram(player); // TODO this should set connected bit too
    }
}

void pad_umount(uint8_t idx)
{
    // Find the descriptor by dev_addr and idx
    int player = pad_find_player_by_idx(idx);
    if (player < 0)
        return;
    pad_descriptor_t *desc = &pad_players[player];
    desc->valid = false;
    desc->idx = 0;
    pad_reset_xram(player);
}

void pad_report(uint8_t idx, uint8_t const *report, uint16_t len)
{
    int player = pad_find_player_by_idx(idx);
    if (player < 0)
        return;
    pad_descriptor_t *desc = &pad_players[player];

    // Skip report ID check if no report ID is expected, or validate if one is expected
    const uint8_t *report_data = report;
    uint16_t report_data_len = len;
    if (desc->report_id != 0)
    {
        if (len == 0 || report[0] != desc->report_id)
        {
            // DBG("pad_report: Report ID mismatch. Expected %d, got %d\n", desc->report_id, len > 0 ? report[0] : -1);
            return;
        }
        report_data = &report[1]; // Skip report ID byte
        report_data_len = len - 1;
    }

    pad_gamepad_report_t gamepad_report;
    pad_parse_report_to_gamepad(player, report_data, report_data_len, &gamepad_report);

    if (pad_xram != 0xFFFF)
    {
        // pad_gamepad_report_t gamepad_report;
        // pad_parse_report_to_gamepad(player, report_data, report_data_len, &gamepad_report);
        memcpy(&xram[pad_xram + player * (sizeof(pad_gamepad_report_t))],
               &gamepad_report, sizeof(pad_gamepad_report_t));
    }
}

bool pad_is_valid(uint8_t idx)
{
    return pad_find_player_by_idx(idx) >= 0;
}

void pad_report_xbox_controller(uint8_t idx, uint8_t const *report, uint16_t len)
{
    DBG("pad_report_xbox_controller: Received report from idx %d, len %d\n", idx, len);
    pad_report(idx, report, len);
}
