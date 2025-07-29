/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb_config.h"
#include "hid/pad.h"
#include "hid/des.h"
#include "sys/mem.h"
#include <stdlib.h>
#include <string.h>

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_PAD)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// dpad bits: 0-up, 1-down, 2-left, 3-right
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
// Apps should use analog values if they want to tighten it up.
#define PAD_DEADZONE 32

static uint16_t pad_xram;
static des_gamepad_t pad_players[PAD_MAX_PLAYERS];

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
        if (pad_players[i].slot == slot && pad_players[i].valid)
            return i;
    return -1;
}

static void pad_parse_report_to_gamepad(int player_idx, uint8_t const *report, uint16_t report_len, pad_report_t *gamepad_report)
{
    // Default empty gamepad report
    memset(gamepad_report, 0, sizeof(pad_report_t));

    // Add feature bits to dpad
    des_gamepad_t *gamepad = &pad_players[player_idx];
    if (gamepad->valid)
        gamepad_report->dpad |= 0x80;
    if (gamepad->sony)
        gamepad_report->dpad |= 0x40;

    // A blank report was requested
    if (report_len == 0)
        return;

    // Extract analog sticks
    if (gamepad->x_size > 0)
    {
        uint32_t raw_x = pad_extract_bits(report, report_len, gamepad->x_offset, gamepad->x_size);
        gamepad_report->lx = pad_scale_analog_signed(raw_x, gamepad->x_size, gamepad->x_min, gamepad->x_max);
    }
    if (gamepad->y_size > 0)
    {
        uint32_t raw_y = pad_extract_bits(report, report_len, gamepad->y_offset, gamepad->y_size);
        gamepad_report->ly = pad_scale_analog_signed(raw_y, gamepad->y_size, gamepad->y_min, gamepad->y_max);
    }
    if (gamepad->z_size > 0)
    {
        uint32_t raw_z = pad_extract_bits(report, report_len, gamepad->z_offset, gamepad->z_size);
        gamepad_report->rx = pad_scale_analog_signed(raw_z, gamepad->z_size, gamepad->z_min, gamepad->z_max);
    }
    if (gamepad->rz_size > 0)
    {
        uint32_t raw_rz = pad_extract_bits(report, report_len, gamepad->rz_offset, gamepad->rz_size);
        gamepad_report->ry = pad_scale_analog_signed(raw_rz, gamepad->rz_size, gamepad->rz_min, gamepad->rz_max);
    }

    // Extract triggers
    if (gamepad->rx_size > 0)
    {
        uint32_t raw_rx = pad_extract_bits(report, report_len, gamepad->rx_offset, gamepad->rx_size);
        gamepad_report->lt = pad_scale_analog(raw_rx, gamepad->rx_size, gamepad->rx_min, gamepad->rx_max);
    }
    if (gamepad->ry_size > 0)
    {
        uint32_t raw_ry = pad_extract_bits(report, report_len, gamepad->ry_offset, gamepad->ry_size);
        gamepad_report->rt = pad_scale_analog(raw_ry, gamepad->ry_size, gamepad->ry_min, gamepad->ry_max);
    }

    // Extract buttons using individual bit offsets
    uint32_t buttons = 0;
    for (int i = 0; i < PAD_MAX_BUTTONS; i++)
        if (pad_extract_bits(report, report_len, gamepad->button_offsets[i], 1))
            buttons |= (1UL << i);
    gamepad_report->button0 = buttons & 0xFF;
    gamepad_report->button1 = (buttons & 0xFF00) >> 8;

    // Extract D-pad/hat
    if (gamepad->hat_size == 4 && gamepad->hat_max - gamepad->hat_min == 7)
    {
        // Convert HID hat format to individual direction bits
        static const uint8_t hat_to_pad[] = {1, 9, 8, 10, 2, 6, 4, 5};
        uint32_t raw_hat = pad_extract_bits(report, report_len, gamepad->hat_offset, gamepad->hat_size);
        unsigned index = raw_hat - gamepad->hat_min;
        if (index < 8)
            gamepad_report->dpad |= hat_to_pad[index];
    }
    else
    {
        gamepad_report->dpad |= (buttons & 0xF0000) >> 16;
    }

    // Generate dpad values for sticks
    uint8_t stick_l = pad_encode_stick(gamepad_report->lx, gamepad_report->ly);
    uint8_t stick_r = pad_encode_stick(gamepad_report->rx, gamepad_report->ry);
    gamepad_report->sticks = stick_l | (stick_r << 4);

    // If L2/R2 buttons pressed without any analog movement
    if ((buttons & (1 << 8)) && (gamepad_report->lt == 0))
        gamepad_report->lt = 255;
    if ((buttons & (1 << 9)) && (gamepad_report->rt == 0))
        gamepad_report->rt = 255;

    // Inject Xbox One home button
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

// Provides first and final updates in xram
static void pad_reset_xram(int player_idx)
{
    if (pad_xram == 0xFFFF)
        return;
    pad_report_t gamepad_report;
    pad_parse_report_to_gamepad(player_idx, 0, 0, &gamepad_report); // get blank
    memcpy(&xram[pad_xram + player_idx * (sizeof(pad_report_t))],
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

bool pad_mount(uint8_t slot, uint8_t const *desc_report, uint16_t desc_len,
               uint16_t vendor_id, uint16_t product_id)
{
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
    DBG("pad_mount: mounting player %d\n", player);

    des_report_descriptor(slot, gamepad, desc_report, desc_len,
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
    des_gamepad_t *gamepad = &pad_players[player];
    gamepad->valid = false;
    gamepad->slot = 0;
    pad_reset_xram(player);
}

void pad_report(uint8_t slot, uint8_t const *report, uint16_t len)
{
    int player = pad_find_player_by_slot(slot);
    if (player < 0)
        return;
    des_gamepad_t *gamepad = &pad_players[player];

    // Skip report ID check if no report ID is expected, or validate if one is expected
    const uint8_t *report_data = report;
    uint16_t report_data_len = len;
    if (gamepad->report_id != 0)
    {
        if (len == 0 || report[0] != gamepad->report_id)
            return;
        // Skip report ID byte
        report_data = &report[1];
        report_data_len = len - 1;
    }

    // Parse report and send it to xram
    if (pad_xram != 0xFFFF)
    {
        pad_report_t gamepad_report;
        pad_parse_report_to_gamepad(player, report_data, report_data_len, &gamepad_report);
        memcpy(&xram[pad_xram + player * (sizeof(pad_report_t))],
               &gamepad_report, sizeof(pad_report_t));
    }
}

bool pad_is_valid(uint8_t slot)
{
    return pad_find_player_by_slot(slot) >= 0;
}

void pad_home_button(uint8_t slot, bool pressed)
{
    int player = pad_find_player_by_slot(slot);
    if (player < 0)
        return;
    des_gamepad_t *gamepad = &pad_players[player];

    // Inject out of band home button into reports
    gamepad->home_pressed = pressed;

    // Update the home button bit in xram
    if (pad_xram != 0xFFFF)
    {
        uint8_t *button1 = &xram[pad_xram + player * (sizeof(pad_report_t)) + 3];
        if (pressed)
            *button1 |= (1 << 4);
        else
            *button1 &= ~(1 << 4);
    }
}

int pad_get_player_num(uint8_t slot)
{
    return pad_find_player_by_slot(slot);
}
