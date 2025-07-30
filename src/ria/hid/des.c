/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"
#include "hid/des.h"

#if defined(DEBUG_HID_USB) || defined(DEBUG_RIA_HID_DES)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static inline int32_t des_extend_signed(uint32_t raw_value, uint8_t bit_size)
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
uint32_t des_extract_bits(const uint8_t *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size)
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

int32_t des_extract_signed(const uint8_t *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size)
{
    return des_extend_signed(des_extract_bits(report, report_len, bit_offset, bit_size), bit_size);
}

uint8_t des_scale_analog(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max)
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

int8_t des_scale_analog_signed(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max)
{
    // Handle reversed polarity
    bool reversed = logical_min > logical_max;
    int32_t min = reversed ? logical_max : logical_min;
    int32_t max = reversed ? logical_min : logical_max;

    // Sign-extend raw_value if needed
    int32_t value;
    if (min < 0 && bit_size < 32)
    {
        value = des_extend_signed(raw_value, bit_size);
    }
    else
    {
        value = (int32_t)raw_value;
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
