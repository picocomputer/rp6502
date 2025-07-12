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

// Sony DS4 report layout
// https://www.psdevwiki.com/ps4/DS4-USB
// typedef struct TU_ATTR_PACKED
// {
//     uint8_t lx, ly, rx, ry; // analog sticks

//     struct
//     {
//         uint8_t dpad : 4; // (8=released, 7=NW, 6=W, 5=SW, 4=S, 3=SE, 2=E, 1=NE, 0=N)
//         uint8_t square : 1;
//         uint8_t cross : 1;
//         uint8_t circle : 1;
//         uint8_t triangle : 1;
//     };

//     struct
//     {
//         uint8_t l1 : 1;
//         uint8_t r1 : 1;
//         uint8_t l2 : 1;
//         uint8_t r2 : 1;
//         uint8_t share : 1;
//         uint8_t option : 1;
//         uint8_t l3 : 1;
//         uint8_t r3 : 1;
//     };

//     struct
//     {
//         uint8_t psbtn : 1;
//         uint8_t tpadbtn : 1;
//         uint8_t counter : 6;
//     };

//     uint8_t l2_trigger;
//     uint8_t r2_trigger;

// } sony_ds4_report_t;

#define PAD_TIMEOUT_TIME_MS 10

static absolute_time_t pad_p1_timer;
static absolute_time_t pad_p2_timer;
static uint8_t pad_p1_dev_addr;
static uint8_t pad_p2_dev_addr;
static uint16_t pad_xram = 0xFFFF;
static pad_descriptor_t pad_descriptors[CFG_TUH_HID] = {0};

static void pad_disconnect_check(void)
{
    // Set dpad invalid to indicate no controller detected
    if (pad_xram != 0xFFFF)
    {
        if (absolute_time_diff_us(get_absolute_time(), pad_p1_timer) < 0)
            xram[pad_xram + 6] = 0x0F;
        if (absolute_time_diff_us(get_absolute_time(), pad_p2_timer) < 0)
            xram[pad_xram + sizeof(hid_gamepad_report_t) + 6] = 0x0F;
    }
}

static pad_descriptor_t *pad_get_descriptor(uint8_t dev_addr)
{
    // Find descriptor for this specific device address
    for (uint8_t i = 0; i < CFG_TUH_HID; i++)
    {
        if (pad_descriptors[i].valid && pad_descriptors[i].dev_addr == dev_addr)
        {
            return &pad_descriptors[i];
        }
    }
    return NULL;
}

static uint32_t pad_extract_bits(uint8_t const *report, uint8_t bit_offset, uint8_t bit_size)
{
    if (bit_size == 0 || bit_size > 32)
        return 0;

    uint8_t byte_offset = bit_offset / 8;
    uint8_t bit_shift = bit_offset % 8;
    uint32_t value = 0;

    // Extract value across multiple bytes if needed
    for (uint8_t i = 0; i < (bit_size + 7) / 8 && i < 4; i++)
    {
        uint32_t v;
        memcpy(&v, &report[byte_offset + i], sizeof(v));
        value |= v << (i * 8);
    }

    // Shift and mask to get the desired bits
    value >>= bit_shift;
    if (bit_size < 32)
    {
        value &= (1UL << bit_size) - 1;
    }

    return value;
}

static void pad_parse_report_to_gamepad(uint8_t dev_addr, uint8_t const *report, hid_gamepad_report_t *gamepad_report)
{
    pad_descriptor_t *desc = pad_get_descriptor(dev_addr);
    if (!desc)
        return;

    // DBG("pad_parse_report_to_gamepad");

    // Clear the gamepad report
    memset(gamepad_report, 0, sizeof(hid_gamepad_report_t));

    // return; //////////////////

    // Extract analog sticks
    if (desc->x_size > 0)
    {
        uint32_t raw_x = pad_extract_bits(report, desc->x_offset, desc->x_size);
        // Convert to signed 8-bit value (assuming 8-bit or larger input)
        gamepad_report->x = (int8_t)((raw_x >> (desc->x_size > 8 ? desc->x_size - 8 : 0)) - 128);
    }

    if (desc->y_size > 0)
    {
        uint32_t raw_y = pad_extract_bits(report, desc->y_offset, desc->y_size);
        gamepad_report->y = (int8_t)((raw_y >> (desc->y_size > 8 ? desc->y_size - 8 : 0)) - 128);
    }

    if (desc->z_size > 0)
    {
        uint32_t raw_z = pad_extract_bits(report, desc->z_offset, desc->z_size);
        gamepad_report->z = (int8_t)((raw_z >> (desc->z_size > 8 ? desc->z_size - 8 : 0)) - 128);
    }

    if (desc->rz_size > 0)
    {
        uint32_t raw_rz = pad_extract_bits(report, desc->rz_offset, desc->rz_size);
        gamepad_report->rz = (int8_t)((raw_rz >> (desc->rz_size > 8 ? desc->rz_size - 8 : 0)) - 128);
    }

    // Extract triggers
    if (desc->rx_size > 0)
    {
        uint32_t raw_rx = pad_extract_bits(report, desc->rx_offset, desc->rx_size);
        gamepad_report->rx = (int8_t)(raw_rx >> (desc->rx_size > 8 ? desc->rx_size - 8 : 0));
    }

    if (desc->ry_size > 0)
    {
        uint32_t raw_ry = pad_extract_bits(report, desc->ry_offset, desc->ry_size);
        gamepad_report->ry = (int8_t)(raw_ry >> (desc->ry_size > 8 ? desc->ry_size - 8 : 0));
    }

    // Extract D-pad/hat
    if (desc->hat_size > 0)
    {
        uint32_t raw_hat = pad_extract_bits(report, desc->hat_offset, desc->hat_size);
        gamepad_report->hat = (uint8_t)raw_hat;
    }

    // Extract buttons
    if (desc->buttons_size > 0)
    {
        uint32_t raw_buttons = pad_extract_bits(report, desc->buttons_offset, desc->buttons_size);
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
}

void pad_task(void)
{
    pad_disconnect_check();
}

bool pad_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - sizeof(hid_gamepad_report_t) * 2)
        return false;
    pad_xram = word;
    pad_disconnect_check();
    return true;
}

bool pad_parse_descriptor(uint8_t dev_addr, uint8_t const *desc_report, uint16_t desc_len)
{
    return des_parse_report_descriptor(pad_descriptors, CFG_TUH_HID, dev_addr, desc_report, desc_len);
}

void pad_cleanup_descriptor(uint8_t dev_addr)
{
    // Find and invalidate descriptor for this device
    for (uint8_t i = 0; i < CFG_TUH_HID; i++)
    {
        if (pad_descriptors[i].valid && pad_descriptors[i].dev_addr == dev_addr)
        {
            pad_descriptors[i].valid = false;
            pad_descriptors[i].dev_addr = 0;
            break;
        }
    }

    // Clean up player assignments if this device was assigned
    if (pad_p1_dev_addr == dev_addr)
    {
        pad_p1_dev_addr = 0;
        pad_p1_timer = nil_time; // Force immediate timeout
    }
    if (pad_p2_dev_addr == dev_addr)
    {
        pad_p2_dev_addr = 0;
        pad_p2_timer = nil_time; // Force immediate timeout
    }
}

void pad_report(uint8_t dev_addr, uint8_t const *report)
{
    // We should probably check VIDs/PIDs or something
    if (report[0] != 1)
        return;

    uint8_t player = 0;
    absolute_time_t now = get_absolute_time();

    if (absolute_time_diff_us(now, pad_p1_timer) >= 0)
        if (pad_p1_dev_addr == dev_addr)
            player = 1;

    if (absolute_time_diff_us(now, pad_p2_timer) >= 0)
        if (pad_p2_dev_addr == dev_addr)
            player = 2;

    if (!player)
    {
        if (absolute_time_diff_us(now, pad_p1_timer) < 0)
            player = 1;
        else if (absolute_time_diff_us(now, pad_p2_timer) < 0)
            player = 2;
    }

    if (player == 1)
    {
        pad_p1_timer = make_timeout_time_ms(PAD_TIMEOUT_TIME_MS);
        pad_p1_dev_addr = dev_addr;
        if (pad_xram != 0xFFFF)
        {
            hid_gamepad_report_t gamepad_report;
            pad_parse_report_to_gamepad(dev_addr, report, &gamepad_report);
            memcpy(&xram[pad_xram], &gamepad_report, sizeof(hid_gamepad_report_t));
        }
    }

    if (player == 2)
    {
        pad_p2_timer = make_timeout_time_ms(PAD_TIMEOUT_TIME_MS);
        pad_p2_dev_addr = dev_addr;
        if (pad_xram != 0xFFFF)
        {
            hid_gamepad_report_t gamepad_report;
            pad_parse_report_to_gamepad(dev_addr, report, &gamepad_report);
            memcpy(&xram[pad_xram + sizeof(hid_gamepad_report_t)], &gamepad_report, sizeof(hid_gamepad_report_t));
        }
    }
}
