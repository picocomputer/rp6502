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

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_PAD)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// hat bits: 0-up, 1-down, 2-left, 3-right
// Feature bit 0x80 is on when valid controller connected
// Feature bit 0x40 is on when Sony-style controller detected
typedef struct TU_ATTR_PACKED
{
    uint8_t hat;     // hat (0x0F) and feature (0xF0) bits
    uint8_t sticks;  // left (0x0F) and right (0xF0) sticks
    uint8_t button0; // buttons
    uint8_t button1; // buttons
    int8_t lx;       // left analog-stick
    int8_t ly;       // left analog-stick
    int8_t rx;       // right analog-stick
    int8_t ry;       // right analog-stick
    uint8_t lt;      // analog left trigger
    uint8_t rt;      // analog right trigger
} pad_gamepad_report_t;

// Deadzone is generous enough for moderately worn sticks.
// Apps should use analog values if they want to tighten it up.
#define PAD_DEADZONE 32

static uint16_t pad_xram;
static des_gamepad_t pad_players[PAD_MAX_PLAYERS];

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
    // Check for polarity reversal (logical_min > logical_max)
    bool reversed = logical_min > logical_max;
    int32_t actual_min = reversed ? logical_max : logical_min;
    int32_t actual_max = reversed ? logical_min : logical_max;

    // Handle signed values by treating them as such
    int32_t signed_value;
    if (bit_size <= 8)
    {
        if (logical_min >= 0)
            signed_value = (uint8_t)raw_value;
        else
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
    if (actual_min < 0 && bit_size < 32)
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

    // Clamp to actual logical range
    if (signed_value < actual_min)
        signed_value = actual_min;
    if (signed_value > actual_max)
        signed_value = actual_max;

    // Scale to 0-255 range
    int32_t range = actual_max - actual_min;
    if (range == 0)
        return 127; // Avoid division by zero

    int32_t normalized = signed_value - actual_min;
    uint8_t result = (uint8_t)((normalized * 255) / range);

    // Apply polarity reversal if needed
    if (reversed)
        result = 255 - result;

    return result;
}

static int8_t pad_scale_analog_signed(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max)
{
    // Fast path for common cases
    if (logical_min == 0 && logical_max == 255)
    {
        // Common unsigned 8-bit case: map 0-255 to -128 to 127
        return (int8_t)(raw_value - 128);
    }
    if (logical_min == -128 && logical_max == 127)
    {
        // Already in target range
        return (int8_t)raw_value;
    }

    // Check for polarity reversal before swapping
    bool reversed = logical_min > logical_max;
    if (reversed)
    {
        int32_t temp = logical_min;
        logical_min = logical_max;
        logical_max = temp;
    }

    // Fast sign extension for common bit sizes
    int32_t signed_value;
    if (bit_size <= 8)
        signed_value = (int8_t)raw_value;
    else if (bit_size <= 16)
        signed_value = (int16_t)raw_value;
    else
    {
        // Manual sign extension for other sizes
        if (logical_min < 0 && (raw_value & (1UL << (bit_size - 1))))
            signed_value = (int32_t)(raw_value | (~((1UL << bit_size) - 1)));
        else
            signed_value = (int32_t)raw_value;
    }

    // Clamp to range
    if (signed_value < logical_min)
        signed_value = logical_min;
    if (signed_value > logical_max)
        signed_value = logical_max;

    // Fast scaling calculation
    int32_t range = logical_max - logical_min;
    if (range == 0)
        return 0;

    int8_t result;
    if (logical_min >= 0)
    {
        // Unsigned logical range: center at 0
        int32_t center = logical_min + (range >> 1);
        int32_t offset = signed_value - center;
        // Scale the offset to fit in -128 to 127 range
        result = (int8_t)((offset * 255) / range);
    }
    else
    {
        // Signed logical range: direct mapping
        int32_t normalized = signed_value - logical_min;
        result = (int8_t)((normalized * 255) / range - 128);
    }

    // Apply polarity reversal if needed
    if (reversed)
        result = -result - 1; // Proper reversal for signed values

    return result;
}

static uint8_t pad_encode_stick(int8_t x, int8_t y)
{
    // Deadzone check - simple and reliable
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

static int pad_find_player_by_idx(uint8_t idx)
{
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
        if (pad_players[i].idx == idx && pad_players[i].valid)
            return i;
    return -1;
}

static void pad_parse_report_to_gamepad(int player_idx, uint8_t const *report, uint16_t report_len, pad_gamepad_report_t *gamepad_report)
{

    // Default empty gamepad report
    memset(gamepad_report, 0, sizeof(pad_gamepad_report_t));

    // Add feature bits to hat
    des_gamepad_t *gamepad = &pad_players[player_idx];
    if (gamepad->valid)
        gamepad_report->hat |= 0x80;
    if (gamepad->sony)
        gamepad_report->hat |= 0x40;

    // A blank report was requested
    if (report_len == 0)
        return;

    // Extract analog sticks
    if (gamepad->x_size > 0)
    {
        uint32_t raw_x = pad_extract_bits(report, report_len, gamepad->x_offset, gamepad->x_size);
        gamepad_report->lx = pad_scale_analog_signed(raw_x, gamepad->x_size, gamepad->x_logical_min, gamepad->x_logical_max);
    }
    if (gamepad->y_size > 0)
    {
        uint32_t raw_y = pad_extract_bits(report, report_len, gamepad->y_offset, gamepad->y_size);
        gamepad_report->ly = pad_scale_analog_signed(raw_y, gamepad->y_size, gamepad->y_logical_min, gamepad->y_logical_max);
    }
    if (gamepad->z_size > 0)
    {
        uint32_t raw_z = pad_extract_bits(report, report_len, gamepad->z_offset, gamepad->z_size);
        gamepad_report->rx = pad_scale_analog_signed(raw_z, gamepad->z_size, gamepad->z_logical_min, gamepad->z_logical_max);
    }
    if (gamepad->rz_size > 0)
    {
        uint32_t raw_rz = pad_extract_bits(report, report_len, gamepad->rz_offset, gamepad->rz_size);
        gamepad_report->ry = pad_scale_analog_signed(raw_rz, gamepad->rz_size, gamepad->rz_logical_min, gamepad->rz_logical_max);
    }

    // Extract triggers
    if (gamepad->rx_size > 0)
    {
        uint32_t raw_rx = pad_extract_bits(report, report_len, gamepad->rx_offset, gamepad->rx_size);
        gamepad_report->lt = pad_scale_analog(raw_rx, gamepad->rx_size, gamepad->rx_logical_min, gamepad->rx_logical_max);
    }
    if (gamepad->ry_size > 0)
    {
        uint32_t raw_ry = pad_extract_bits(report, report_len, gamepad->ry_offset, gamepad->ry_size);
        gamepad_report->rt = pad_scale_analog(raw_ry, gamepad->ry_size, gamepad->ry_logical_min, gamepad->ry_logical_max);
    }

    // Extract buttons using individual bit offsets
    uint32_t buttons = 0;
    for (int i = 0; i < PAD_MAX_BUTTONS; i++)
        if (pad_extract_bits(report, report_len, gamepad->button_offsets[i], 1))
            buttons |= (1UL << i);
    gamepad_report->button0 = buttons & 0xFF;
    gamepad_report->button1 = (buttons & 0xFF00) >> 8;

    // Extract D-pad/hat
    if (gamepad->hat_size == 4 && gamepad->hat_logical_min == 0 && gamepad->hat_logical_max == 7)
    {
        // Standard HID hat switch - convert to individual button format
        uint32_t raw_hat = pad_extract_bits(report, report_len, gamepad->hat_offset, gamepad->hat_size);
        // Convert HID hat format (0-7 clockwise, 8=none) to individual direction bits
        switch (raw_hat)
        {
        case 0: // North
            gamepad_report->hat |= 1;
            break;
        case 1: // North-East
            gamepad_report->hat |= 9;
            break;
        case 2: // East
            gamepad_report->hat |= 8;
            break;
        case 3: // South-East
            gamepad_report->hat |= 10;
            break;
        case 4: // South
            gamepad_report->hat |= 2;
            break;
        case 5: // South-West
            gamepad_report->hat |= 6;
            break;
        case 6: // West
            gamepad_report->hat |= 4;
            break;
        case 7: // North-West
            gamepad_report->hat |= 5;
            break;
        default: // No direction (8) or invalid
            gamepad_report->hat |= 0;
            break;
        }
    }
    else
    {
        gamepad_report->hat |= (buttons & 0xF0000) >> 16;
    }

    // Generate hat values for sticks
    uint8_t stick_l = pad_encode_stick(gamepad_report->lx, gamepad_report->ly);
    uint8_t stick_r = pad_encode_stick(gamepad_report->rx, gamepad_report->ry);
    gamepad_report->sticks = stick_l | (stick_r << 4);

    // If L2/R2 buttons pressed without any analog movement
    if ((buttons & (1 << 8)) && (gamepad_report->lt == 0))
        gamepad_report->lt = 255;
    if ((buttons & (1 << 9)) && (gamepad_report->rt == 0))
        gamepad_report->rt = 255;

    // Inject xbox one home button
    if (gamepad->home_pressed)
        gamepad_report->button1 |= (1 << 4); // Home

    // If L2/R2 analog movement, ensure button press
    if (gamepad_report->lt > PAD_DEADZONE)
        gamepad_report->button1 |= (1 << 0); // L2
    if (gamepad_report->rt > PAD_DEADZONE)
        gamepad_report->button1 |= (1 << 1); // R2
}

void pad_init(void)
{
    pad_stop();
}

void pad_stop(void)
{
    pad_xram = 0xFFFF;
}

static void pad_reset_xram(int player_idx)
{
    if (pad_xram == 0xFFFF)
        return;
    pad_gamepad_report_t gamepad_report;
    pad_parse_report_to_gamepad(player_idx, 0, 0, &gamepad_report); // get blank
    memcpy(&xram[pad_xram + player_idx * (sizeof(pad_gamepad_report_t))],
           &gamepad_report, sizeof(pad_gamepad_report_t));
}

bool pad_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - (sizeof(pad_gamepad_report_t)) * PAD_MAX_PLAYERS)
        return false;
    pad_xram = word;
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
        pad_reset_xram(i);
    return true;
}

bool pad_mount(uint8_t idx, uint8_t const *desc_report, uint16_t desc_len,
               uint8_t dev_addr, uint16_t vendor_id, uint16_t product_id)
{
    // Find an available descriptor slot
    des_gamepad_t *gamepad = NULL;
    int player;
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
    {
        if (!pad_players[i].valid)
        {
            gamepad = &pad_players[i];
            player = i;
            break;
        }
    }

    if (!gamepad)
    {
        DBG("pad_mount: No available descriptor slots, max players reached\n");
        return false;
    }

    des_report_descriptor(gamepad, desc_report, desc_len,
                          dev_addr, vendor_id, product_id);

    // Try to assign to an available player slot
    if (gamepad->valid)
    {
        gamepad->idx = idx;     // Store the interface index
        pad_reset_xram(player); // TODO this should set connected bit too
        return true;
    }
    return false;
}

void pad_umount(uint8_t idx)
{
    // Find the descriptor by dev_addr and idx
    int player = pad_find_player_by_idx(idx);
    if (player < 0)
        return;
    des_gamepad_t *gamepad = &pad_players[player];
    gamepad->valid = false;
    gamepad->idx = 0;
    pad_reset_xram(player);
}

void pad_report(uint8_t idx, uint8_t const *report, uint16_t len)
{
    int player = pad_find_player_by_idx(idx);
    if (player < 0)
        return;
    des_gamepad_t *gamepad = &pad_players[player];

    // Skip report ID check if no report ID is expected, or validate if one is expected
    const uint8_t *report_data = report;
    uint16_t report_data_len = len;
    if (gamepad->report_id != 0)
    {
        // DBG("report_id expected \\x%02X got \\x%02X", gamepad->report_id, report[0]);
        if (len == 0 || report[0] != gamepad->report_id)
            return;
        // Skip report ID byte
        report_data = &report[1];
        report_data_len = len - 1;
    }

    // Parse report and send it to xram
    if (pad_xram != 0xFFFF)
    {
        pad_gamepad_report_t gamepad_report;
        pad_parse_report_to_gamepad(player, report_data, report_data_len, &gamepad_report);
        memcpy(&xram[pad_xram + player * (sizeof(pad_gamepad_report_t))],
               &gamepad_report, sizeof(pad_gamepad_report_t));
    }
}

bool pad_is_valid(uint8_t idx)
{
    return pad_find_player_by_idx(idx) >= 0;
}

void pad_home_button(uint8_t idx, bool pressed)
{
    int player = pad_find_player_by_idx(idx);
    if (player < 0)
        return;
    des_gamepad_t *gamepad = &pad_players[player];

    // Inject out of band home button into reports
    gamepad->home_pressed = pressed;

    // Update the correct home button bit in xram
    if (pad_xram != 0xFFFF)
    {
        uint8_t *button1 = &xram[pad_xram + player * (sizeof(pad_gamepad_report_t)) + 3];
        if (pressed)
            *button1 |= (1 << 4);
        else
            *button1 &= ~(1 << 4);
    }
}
