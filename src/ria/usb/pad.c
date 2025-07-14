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
    if ((x > -32 && x < 32) && (y > -32 && y < 32))
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

    // Extract analog sticks
    if (desc->x_size > 0)
    {
        uint32_t raw_x = pad_extract_bits(report, report_len, desc->x_offset, desc->x_size);
        gamepad_report->x = (uint8_t)(raw_x >> (desc->x_size > 8 ? desc->x_size - 8 : 0));
    }
    if (desc->y_size > 0)
    {
        uint32_t raw_y = pad_extract_bits(report, report_len, desc->y_offset, desc->y_size);
        gamepad_report->y = (uint8_t)(raw_y >> (desc->y_size > 8 ? desc->y_size - 8 : 0));
    }
    if (desc->z_size > 0)
    {
        uint32_t raw_z = pad_extract_bits(report, report_len, desc->z_offset, desc->z_size);
        gamepad_report->z = (uint8_t)(raw_z >> (desc->z_size > 8 ? desc->z_size - 8 : 0));
    }
    if (desc->rz_size > 0)
    {
        uint32_t raw_rz = pad_extract_bits(report, report_len, desc->rz_offset, desc->rz_size);
        gamepad_report->rz = (uint8_t)(raw_rz >> (desc->rz_size > 8 ? desc->rz_size - 8 : 0));
    }

    // Extract triggers
    if (desc->rx_size > 0)
    {
        uint32_t raw_rx = pad_extract_bits(report, report_len, desc->rx_offset, desc->rx_size);
        gamepad_report->rx = (uint8_t)(raw_rx >> (desc->rx_size > 8 ? desc->rx_size - 8 : 0));
    }
    if (desc->ry_size > 0)
    {
        uint32_t raw_ry = pad_extract_bits(report, report_len, desc->ry_offset, desc->ry_size);
        gamepad_report->ry = (uint8_t)(raw_ry >> (desc->ry_size > 8 ? desc->ry_size - 8 : 0));
    }

    // Extract D-pad/hat
    if (desc->hat_size > 0)
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
    for (int i = 0; i < PAD_MAX_BUTTONS && desc->button_offsets[i] != 0xFF; i++)
        if (pad_extract_bits(report, report_len, desc->button_offsets[i], 1))
            buttons |= (1UL << i);
    gamepad_report->button0 = buttons & 0xFF;
    gamepad_report->button1 = (buttons & 0xFF00) >> 8;

    // Generate hat values for sticks
    uint8_t hat_l = pad_encode_hat(gamepad_report->x, gamepad_report->y);
    uint8_t hat_r = pad_encode_hat(gamepad_report->z, gamepad_report->rz);
    gamepad_report->sticks = hat_l | (hat_r << 4);

    // TODO normalize L2/R2
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
