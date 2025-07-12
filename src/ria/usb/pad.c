/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "usb/pad.h"
#include "usb/des.h"
#include "sys/mem.h"

#define DEBUG_RIA_USB_PAD /////////////////////////////

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_PAD)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...) \
    {            \
    }
#endif

typedef struct TU_ATTR_PACKED
{
    uint8_t x;        ///< Delta x  movement of left analog-stick
    uint8_t y;        ///< Delta y  movement of left analog-stick
    uint8_t z;        ///< Delta z  movement of right analog-joystick
    uint8_t rz;       ///< Delta Rz movement of right analog-joystick
    uint8_t rx;       ///< Delta Rx movement of analog left trigger
    uint8_t ry;       ///< Delta Ry movement of analog right trigger
    uint8_t hat;      ///< Buttons mask for currently pressed buttons in the DPad/hat
    uint32_t buttons; ///< Buttons mask for currently pressed buttons
} pad_gamepad_report_t;

#define PAD_TIMEOUT_TIME_MS 100   // Increased from 10ms for better stability
#define PAD_DPAD_INVALID_OFFSET 6 // Offset for D-pad invalid marker

static int16_t pad_p1_dev_idx;
static int16_t pad_p2_dev_idx;
static uint16_t pad_xram = 0xFFFF;
static pad_descriptor_t pad_descriptors[CFG_TUH_HID] = {0};

static void pad_disconnect_check(void)
{
    // Set dpad invalid to indicate no controller detected
    if (pad_xram != 0xFFFF)
    {
        if (pad_p1_dev_idx == -1)
            xram[pad_xram + PAD_DPAD_INVALID_OFFSET] = 0x0F;
        if (pad_p2_dev_idx == -1)
            xram[pad_xram + sizeof(pad_gamepad_report_t) + PAD_DPAD_INVALID_OFFSET] = 0x0F;
    }
}

static uint32_t pad_extract_bits(uint8_t const *report, uint16_t report_len, uint8_t bit_offset, uint8_t bit_size)
{
    if (bit_size == 0 || bit_size > 32)
        return 0;

    uint8_t byte_offset = bit_offset / 8;
    uint8_t bit_shift = bit_offset % 8;

    // Check if we have enough bytes in the report
    uint8_t bytes_needed = (bit_offset + bit_size + 7) / 8;
    if (bytes_needed > report_len)
    {
        DBG("pad_extract_bits: Not enough data in report. Need %d bytes, have %d\n", bytes_needed, report_len);
        return 0;
    }

    uint32_t value = 0;

    // Extract value across multiple bytes if needed
    for (uint8_t i = 0; i < (bit_size + 7) / 8 && i < 4 && (byte_offset + i) < report_len; i++)
    {
        value |= ((uint32_t)report[byte_offset + i]) << (i * 8);
    }

    // Shift and mask to get the desired bits
    value >>= bit_shift;
    if (bit_size < 32)
    {
        value &= (1UL << bit_size) - 1;
    }

    return value;
}

static void pad_parse_report_to_gamepad(uint8_t idx, uint8_t const *report, uint16_t report_len, pad_gamepad_report_t *gamepad_report)
{
    pad_descriptor_t *desc = &pad_descriptors[idx];

    // DBG("pad_parse_report_to_gamepad");

    // Clear the gamepad report
    memset(gamepad_report, 0, sizeof(pad_gamepad_report_t));

    // DBG("pad_parse_report_to_gamepad: dev_addr=%d, report_len=%d\n", dev_addr, report_len);

    // Extract analog sticks
    if (desc->x_size > 0)
    {
        uint32_t raw_x = pad_extract_bits(report, report_len, desc->x_offset, desc->x_size);
        // Convert to unsigned 8-bit value (0-255)
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
        gamepad_report->hat = (uint8_t)raw_hat;
    }

    // Extract buttons
    if (desc->buttons_size > 0)
    {
        uint32_t raw_buttons = pad_extract_bits(report, report_len, desc->buttons_offset, desc->buttons_size);
        gamepad_report->buttons = raw_buttons;
    }
}

void pad_init(void)
{
    pad_stop();
    // Clear all descriptors
    memset(pad_descriptors, 0, sizeof(pad_descriptors));
}

void pad_stop(void)
{
    pad_xram = 0xFFFF;
    pad_p1_dev_idx = -1;
    pad_p2_dev_idx = -1;
}

void pad_task(void)
{
}

bool pad_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - sizeof(pad_gamepad_report_t) * 2)
        return false;
    pad_xram = word;
    pad_disconnect_check();
    return true;
}

bool pad_parse_descriptor(uint8_t dev_addr, uint8_t idx, uint8_t const *desc_report, uint16_t desc_len,
                          uint16_t vendor_id, uint16_t product_id)
{
    return des_parse_report_descriptor(&pad_descriptors[idx], dev_addr, desc_report, desc_len,
                                       vendor_id, product_id);
}

void pad_cleanup_descriptor(uint8_t idx)
{
    pad_descriptors[idx].valid = false;
    // Clean up player assignments if this device was assigned
    if (pad_p1_dev_idx == idx)
    {
        pad_p1_dev_idx = -1;
    }
    if (pad_p2_dev_idx == idx)
    {
        pad_p2_dev_idx = -1;
    }
    pad_disconnect_check();
}

void pad_report(uint8_t dev_addr, uint8_t idx, uint8_t const *report, uint16_t len)
{
    // DBG("pad_report: dev_addr=%d, len=%d\n", dev_addr, len);

    pad_descriptor_t *desc = &pad_descriptors[idx];

    if (!desc->valid)
        return;

    // DBG("pad_report: Found descriptor for dev_addr %d, report_id=%d\n", dev_addr, desc->report_id);

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

    uint8_t player = 0;

    // Check if this device is already assigned to a player
    if (pad_p1_dev_idx == idx)
        player = 1;
    else if (pad_p2_dev_idx == idx)
        player = 2;

    // If not assigned, try to assign to an available player slot
    if (!player)
    {
        if (pad_p1_dev_idx == -1)
            player = 1;
        else if (pad_p2_dev_idx == -1)
            player = 2;
    }

    // Exit early if no player slot is available
    if (!player)
    {
        DBG("pad_report: No player slot available for dev_addr %d\n", dev_addr);
        return;
    }

    if (player == 1)
    {
        pad_p1_dev_idx = idx;
        if (pad_xram != 0xFFFF)
        {
            pad_gamepad_report_t gamepad_report;
            pad_parse_report_to_gamepad(idx, report_data, report_data_len, &gamepad_report);
            memcpy(&xram[pad_xram], &gamepad_report, sizeof(pad_gamepad_report_t));
        }
    }

    if (player == 2)
    {
        pad_p2_dev_idx = idx;
        if (pad_xram != 0xFFFF)
        {
            pad_gamepad_report_t gamepad_report;
            pad_parse_report_to_gamepad(idx, report_data, report_data_len, &gamepad_report);
            memcpy(&xram[pad_xram + sizeof(pad_gamepad_report_t)], &gamepad_report, sizeof(pad_gamepad_report_t));
        }
    }
}
