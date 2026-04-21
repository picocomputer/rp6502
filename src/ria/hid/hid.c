/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hid/hid.h"

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

void hid_descriptor_parse(const uint8_t *desc, uint16_t desc_len, hid_field_cb_t cb, void *context)
{
    // Global state
    uint16_t usage_page = 0;
    int32_t logical_min = 0;
    int32_t logical_max = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    uint16_t report_id = 0xFFFF;
    uint16_t bit_pos = 0;

    // Local state (cleared after each Main item)
    uint16_t usages[32];
    const uint8_t max_usages = sizeof(usages) / sizeof(usages[0]);
    uint8_t usage_count = 0;
    uint16_t usage_min = 0;
    uint16_t usage_max = 0;
    bool have_usage_range = false;

    uint16_t pos = 0;
    while (pos < desc_len)
    {
        uint8_t b = desc[pos];
        uint8_t sz = b & 0x03;
        if (sz == 3)
            sz = 4;
        uint8_t type = (b >> 2) & 0x03;
        uint8_t tag = (b >> 4) & 0x0F;

        if (pos + 1 + sz > desc_len)
            break;

        uint32_t val = 0;
        for (uint8_t i = 0; i < sz; i++)
            val |= (uint32_t)desc[pos + 1 + i] << (8 * i);
        int32_t sval = (int32_t)val;
        if (sz > 0 && sz < 4 && (val & (1u << (sz * 8 - 1))))
            sval = (int32_t)(val | (~0u << (sz * 8)));

        switch (type)
        {
        case 1: // Global
            switch (tag)
            {
            case 0:
                usage_page = val;
                break;
            case 1:
                logical_min = sval;
                break;
            case 2:
                logical_max = sval;
                break;
            case 7:
                report_size = val;
                break;
            case 8:
                report_id = val;
                bit_pos = 0;
                break;
            case 9:
                report_count = val;
                break;
            }
            break;

        case 2: // Local
            switch (tag)
            {
            case 0: // Usage
                if (usage_count < max_usages)
                    usages[usage_count++] = val;
                break;
            case 1: // Usage Minimum
                usage_min = val;
                have_usage_range = true;
                break;
            case 2: // Usage Maximum
                usage_max = val;
                break;
            }
            break;

        case 0:           // Main
            if (tag == 8) // Input
            {
                // Expand usage range into usage list
                if (have_usage_range && usage_count == 0)
                    for (uint16_t u = usage_min;
                         u <= usage_max && usage_count < max_usages;
                         u++)
                        usages[usage_count++] = u;

                for (uint32_t i = 0; i < report_count; i++)
                {
                    uint16_t usage = (i < usage_count) ? usages[i] : (usage_count > 0) ? usages[usage_count - 1]
                                                                                       : 0;

                    hid_field_t field = {
                        .usage_page = usage_page,
                        .usage = usage,
                        .report_id = report_id,
                        .bit_pos = bit_pos,
                        .size = report_size,
                        .logical_min = logical_min,
                        .logical_max = logical_max,
                        .input_flags = val,
                    };

                    if (!(val & 1)) // Not constant
                        if (!cb(&field, context))
                            return;

                    bit_pos += report_size;
                }
            }
            // Clear local state after any Main item
            usage_count = 0;
            have_usage_range = false;
            usage_min = 0;
            usage_max = 0;
            break;
        }

        pos += 1 + sz;
    }
}
