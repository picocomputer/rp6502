/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include "core/hid/hid.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include "host.h"

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_HID)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static inline int32_t hid_extend_signed(uint32_t raw_value, uint8_t bit_size)
{
    if (bit_size == 0 || bit_size >= 32)
        return (int32_t)raw_value;

    // Check if the sign bit is set (MSB of the bit_size range)
    uint32_t sign_bit = 1UL << (bit_size - 1);

    if (raw_value & sign_bit)
    {
        // Sign bit is set, extend with 1s
        uint32_t sign_extension = ~((1UL << bit_size) - 1);
        return (int32_t)(raw_value | sign_extension);
    }
    else
    {
        // Sign bit is clear, just mask to ensure clean value
        uint32_t mask = (1UL << bit_size) - 1;
        return (int32_t)(raw_value & mask);
    }
}

uint32_t hid_extract_bits(const uint8_t *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size)
{
    if (!bit_size || bit_size > 32)
        return 0;

    uint16_t start_byte = bit_offset / 8;
    uint8_t start_bit = bit_offset % 8;
    uint16_t end_byte = (bit_offset + bit_size - 1) / 8;

    if (end_byte >= report_len)
        return 0;

    // Extract up to 5 bytes into a 64-bit value (a 32-bit field
    // not aligned to a byte boundary can span 5 bytes)
    uint64_t value = 0;
    for (uint8_t i = 0; i < 5 && (start_byte + i) < report_len; ++i)
        value |= ((uint64_t)report[start_byte + i]) << (8 * i);

    value >>= start_bit;
    if (bit_size < 32)
        value &= (1UL << bit_size) - 1;

    return (uint32_t)value;
}

int32_t hid_extract_signed(const uint8_t *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size)
{
    return hid_extend_signed(hid_extract_bits(report, report_len, bit_offset, bit_size), bit_size);
}

uint8_t hid_scale_analog(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max)
{
    // Handle reversal
    bool reversed = logical_min > logical_max;
    int32_t min = reversed ? logical_max : logical_min;
    int32_t max = reversed ? logical_min : logical_max;

    // Extend sign as needed
    int32_t value;
    if (min < 0 && bit_size < 32)
        value = hid_extend_signed(raw_value, bit_size);
    else
        value = (int32_t)raw_value;

    if (reversed)
        value = max + min - value;

    // Clamp bad input
    if (value < min)
        value = min;
    if (value > max)
        value = max;

    // Guard against overflow wrap when range spans the full int32 space
    int32_t discrete_values = max - min + 1;
    if (!discrete_values)
        return 0;

    return ((value - min) * 256 / discrete_values);
}

int8_t hid_scale_analog_signed(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max)
{
    return hid_scale_analog(raw_value, bit_size, logical_min, logical_max) - 128;
}

// Which drivers kept the device in each slot; 0 is a free slot.
static uint8_t hid_claims[HID_MAX_SLOTS];

uint8_t hid_slot_claims(int slot)
{
    return (slot >= 0 && slot < HID_MAX_SLOTS) ? hid_claims[slot] : 0;
}

int HOST_IN_FLASH("hid_mount") hid_mount(const keyboard_connection_t *keyboard,
                                         const mouse_connection_t *mouse,
                                      const tablet_connection_t *tablet,
                                      const gamepad_connection_t *gamepad,
                                      uint16_t vendor_id, uint16_t product_id,
                                      uint8_t button_type)
{
    int slot = -1;
    for (int i = 0; i < HID_MAX_SLOTS; i++)
        if (!hid_claims[i])
        {
            slot = i;
            break;
        }
    if (slot < 0)
        return -1;

    uint8_t claims = 0;
    if (keyboard && keyboard_mount(slot, keyboard, vendor_id, product_id))
        claims |= HID_CLAIM_KEYBOARD;
    if (mouse && mouse_mount(slot, mouse))
        claims |= HID_CLAIM_MOUSE;
    if (tablet && tablet_mount(slot, tablet))
        claims |= HID_CLAIM_TABLET;
    if (gamepad && gamepad_mount(slot, gamepad, vendor_id, product_id, button_type))
        claims |= HID_CLAIM_PAD;

    hid_claims[slot] = claims;
    return claims ? slot : -1;
}

void hid_report(int slot, const uint8_t *data, uint16_t len)
{
    if (slot < 0 || slot >= HID_MAX_SLOTS)
        return;
    uint8_t claims = hid_claims[slot];
    if (claims & HID_CLAIM_KEYBOARD)
        keyboard_report(slot, data, len);
    if (claims & HID_CLAIM_MOUSE)
        mouse_report(slot, data, len);
    if (claims & HID_CLAIM_TABLET)
        tablet_report(slot, data, len);
    if (claims & HID_CLAIM_PAD)
        gamepad_report(slot, data, len);
}

void hid_umount(int slot)
{
    if (slot < 0 || slot >= HID_MAX_SLOTS)
        return;
    uint8_t claims = hid_claims[slot];
    if (claims & HID_CLAIM_KEYBOARD)
        keyboard_umount(slot);
    if (claims & HID_CLAIM_MOUSE)
        mouse_umount(slot);
    if (claims & HID_CLAIM_TABLET)
        tablet_umount(slot);
    if (claims & HID_CLAIM_PAD)
        gamepad_umount(slot);
    hid_claims[slot] = 0;
}
