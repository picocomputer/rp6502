/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hid/hid.h"

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_HID)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static inline int32_t hid_extend_signed(uint32_t raw_value, uint8_t bit_size)
{
    if (bit_size == 0 || bit_size > 32)
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

    // Extract up to 4 bytes into a 32-bit value
    uint32_t value = 0;
    for (uint8_t i = 0; i < 4 && (start_byte + i) < report_len; ++i)
        value |= ((uint32_t)report[start_byte + i]) << (8 * i);

    value >>= start_bit;
    if (bit_size < 32)
        value &= (1UL << bit_size) - 1;

    return value;
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

    // Handle reversal
    if (reversed)
        value = -value - 1;

    // Clamp bad input
    if (value < min)
        value = min;
    if (value > max)
        value = max;

    // Compute discrete values and short circuit divide by zero
    int32_t discrete_values = max - min + 1;
    if (!discrete_values)
        return 0;

    // Final math
    return ((value + min) * 256 / discrete_values);
}

int8_t hid_scale_analog_signed(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max)
{
    return hid_scale_analog(raw_value, bit_size, logical_min, logical_max) - 128;
}
