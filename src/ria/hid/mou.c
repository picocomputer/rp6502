/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <pico.h>
#include "btstack.h"
#include "tusb_config.h"
#include "hid/mou.h"
#include "sys/mem.h"
#include <string.h>

#define DEBUG_RIA_HID_MOU

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_MOU)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define MOU_MAX_MICE 4

typedef struct
{
    uint8_t buttons;
    uint8_t x;
    uint8_t y;
    uint8_t wheel;
    uint8_t pan;
} mou_xram_t;

static uint16_t mou_xram = 0xFFFF;

static struct
{
    uint16_t x;
    uint16_t y;
    uint16_t wheel;
    uint16_t pan;
} mou_state;

// Mouse descriptors are normalized to this structure.
typedef struct
{
    bool valid;
    uint8_t slot;      // HID protocol drivers use slots assigned in hid.h
    uint8_t report_id; // If non zero, the first report byte must match and will be skipped
    uint16_t button_offsets[8];
    uint16_t x_offset; // X axis
    uint8_t x_size;
    int32_t x_min;
    int32_t x_max;
    uint16_t y_offset; // Y axis
    uint8_t y_size;
    int32_t y_min;
    int32_t y_max;
    uint16_t wheel_offset; // Wheel/scroll wheel
    uint8_t wheel_size;
    int32_t wheel_min;
    int32_t wheel_max;
    uint16_t pan_offset; // Horizontal pan/tilt
    uint8_t pan_size;
    int32_t pan_min;
    int32_t pan_max;
} mou_descriptor_t;

static mou_descriptor_t mou_descriptors[MOU_MAX_MICE];

static int find_descriptor_by_slot(int slot)
{
    for (int i = 0; i < MOU_MAX_MICE; ++i)
    {
        if (mou_descriptors[i].valid && mou_descriptors[i].slot == slot)
            return i;
    }
    return -1;
}

static void mou_update_xram(uint8_t buttons)
{
    if (mou_xram == 0xFFFF)
        return;
    mou_xram_t mouse;
    mouse.buttons = buttons;
    mouse.x = mou_state.x >> 8;
    mouse.y = mou_state.y >> 8;
    mouse.wheel = mou_state.wheel >> 8;
    mouse.pan = mou_state.pan >> 8;
    memcpy(&xram[mou_xram], &mouse, sizeof(mouse));
}

void mou_init(void)
{
    mou_stop();
}

void mou_stop(void)
{
    mou_xram = 0xFFFF;
}

bool mou_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - sizeof(mou_xram_t))
        return false;
    mou_xram = word;
    mou_update_xram(0);
    return true;
}

// Extract bits from HID report data similar to pad.c
static uint32_t mou_extract_bits(const uint8_t *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size)
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
        value |= ((uint32_t)report[start_byte + i]) << (i * 8);

    value >>= start_bit;
    if (bit_size < 32)
        value &= (1ULL << bit_size) - 1;

    return value;
}

// Scale analog values similar to pad.c
static int16_t mou_scale_analog_signed(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max)
{
    // Handle reversed polarity
    bool reversed = logical_min > logical_max;
    int32_t min = reversed ? logical_max : logical_min;
    int32_t max = reversed ? logical_min : logical_max;

    // Sign-extend raw_value if needed
    int32_t value = (int32_t)raw_value;
    if (min < 0 && bit_size < 32)
    {
        uint32_t sign_bit = 1ULL << (bit_size - 1);
        if (value & sign_bit)
            value |= ~((1ULL << bit_size) - 1);
    }

    // Clamp to logical range
    if (value < min)
        value = min;
    if (value > max)
        value = max;

    int32_t range = max - min;
    if (range == 0)
        return 0;

    // Scale to -32768..32767
    int32_t scaled = ((value - min) * 65535 + (range / 2)) / range - 32768;
    int16_t result = (int16_t)scaled;

    // Reverse if needed
    if (reversed)
        result = -result;

    return result;
}

// Parse HID descriptor to extract mouse report structure
static void mou_parse_descriptor(mou_descriptor_t *desc, uint8_t const *desc_data, uint16_t desc_len)
{
    // Initialize all fields
    memset(desc, 0, sizeof(mou_descriptor_t));
    for (int i = 0; i < 8; i++)
        desc->button_offsets[i] = 0xFFFF;

    if (desc_len == 0)
        return;

    // Use BTstack HID parser to parse the descriptor
    btstack_hid_usage_iterator_t iterator;
    btstack_hid_usage_iterator_init(&iterator, desc_data, desc_len, HID_REPORT_TYPE_INPUT);

    while (btstack_hid_usage_iterator_has_more(&iterator))
    {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&iterator, &item);

        // Store report ID if this is the first one we encounter
        if (desc->report_id == 0 && item.report_id != 0xFFFF)
            desc->report_id = item.report_id;

        // Map usages to mouse fields
        if (item.usage_page == 0x01) // Generic Desktop
        {
            switch (item.usage)
            {
            case 0x30: // X axis
                desc->x_offset = item.bit_pos;
                desc->x_size = item.size;
                desc->x_min = iterator.global_logical_minimum;
                desc->x_max = iterator.global_logical_maximum;
                break;
            case 0x31: // Y axis
                desc->y_offset = item.bit_pos;
                desc->y_size = item.size;
                desc->y_min = iterator.global_logical_minimum;
                desc->y_max = iterator.global_logical_maximum;
                break;
            case 0x38: // Wheel
                desc->wheel_offset = item.bit_pos;
                desc->wheel_size = item.size;
                desc->wheel_min = iterator.global_logical_minimum;
                desc->wheel_max = iterator.global_logical_maximum;
                break;
            case 0x3C: // Pan/horizontal wheel
                desc->pan_offset = item.bit_pos;
                desc->pan_size = item.size;
                desc->pan_min = iterator.global_logical_minimum;
                desc->pan_max = iterator.global_logical_maximum;
                break;
            }
        }
        else if (item.usage_page == 0x09) // Button page
        {
            // Map buttons to offsets
            if (item.usage >= 1 && item.usage <= 8)
            {
                desc->button_offsets[item.usage - 1] = item.bit_pos;
            }
        }
    }

    // Validate if we have basic mouse functionality
    desc->valid = (desc->x_size > 0 || desc->y_size > 0 ||
                   desc->button_offsets[0] != 0xFFFF ||
                   desc->button_offsets[1] != 0xFFFF ||
                   desc->button_offsets[2] != 0xFFFF);

    // Debug print parsed descriptor
    DBG("mou_parse_descriptor: report_id=%d, valid=%d\n", desc->report_id, desc->valid);
    DBG("  X: offset=%d, size=%d, min=%d, max=%d\n", desc->x_offset, desc->x_size, desc->x_min, desc->x_max);
    DBG("  Y: offset=%d, size=%d, min=%d, max=%d\n", desc->y_offset, desc->y_size, desc->y_min, desc->y_max);
    DBG("  Wheel: offset=%d, size=%d, min=%d, max=%d\n", desc->wheel_offset, desc->wheel_size, desc->wheel_min, desc->wheel_max);
    DBG("  Pan: offset=%d, size=%d, min=%d, max=%d\n", desc->pan_offset, desc->pan_size, desc->pan_min, desc->pan_max);
    DBG("  Buttons: [%d,%d,%d,%d,%d,%d,%d,%d]\n",
        desc->button_offsets[0], desc->button_offsets[1], desc->button_offsets[2], desc->button_offsets[3],
        desc->button_offsets[4], desc->button_offsets[5], desc->button_offsets[6], desc->button_offsets[7]);
}

bool mou_mount(uint8_t slot, uint8_t const *desc_data, uint16_t desc_len)
{
    int desc_idx = -1;
    for (int i = 0; i < MOU_MAX_MICE; ++i)
        if (!mou_descriptors[i].valid)
        {
            desc_idx = i;
            break;
        }
    if (desc_idx < 0)
        return false;

    mou_descriptor_t *desc = &mou_descriptors[desc_idx];

    // Process raw HID descriptor into desc
    mou_parse_descriptor(desc, desc_data, desc_len);
    desc->slot = slot;

    DBG("mou_mount: slot=%d, valid=%d, x_size=%d, y_size=%d\n",
        slot, desc->valid, desc->x_size, desc->y_size);

    return desc->valid;
}

void mou_umount(uint8_t slot)
{
    int desc_idx = find_descriptor_by_slot(slot);
    if (desc_idx < 0)
        return;
    mou_descriptor_t *desc = &mou_descriptors[desc_idx];
    desc->valid = false;
}

void mou_report(uint8_t slot, void const *data, size_t size)
{
    int desc_idx = find_descriptor_by_slot(slot);
    if (desc_idx < 0)
        return;
    mou_descriptor_t *desc = &mou_descriptors[desc_idx];

    const uint8_t *report_data = (const uint8_t *)data;
    uint16_t report_data_len = size;

    // Skip report ID check if no report ID is expected,
    // or validate if one is expected
    if (desc->report_id != 0)
    {
        if (report_data_len == 0 || report_data[0] != desc->report_id)
            return;
        report_data++;
        report_data_len--;
    }

    // Extract button states
    uint8_t buttons = 0;
    for (int i = 0; i < 8; i++)
    {
        if (desc->button_offsets[i] != 0xFFFF)
        {
            uint32_t button_val = mou_extract_bits(report_data, report_data_len,
                                                   desc->button_offsets[i], 1);
            if (button_val)
                buttons |= (1 << i);
        }
    }

    // Extract movement data
    int16_t delta_x = 0, delta_y = 0, delta_wheel = 0, delta_pan = 0;

    if (desc->x_size > 0)
    {
        uint32_t raw_x = mou_extract_bits(report_data, report_data_len,
                                          desc->x_offset, desc->x_size);
        delta_x = mou_scale_analog_signed(raw_x, desc->x_size, desc->x_min, desc->x_max);
    }

    if (desc->y_size > 0)
    {
        uint32_t raw_y = mou_extract_bits(report_data, report_data_len,
                                          desc->y_offset, desc->y_size);
        delta_y = mou_scale_analog_signed(raw_y, desc->y_size, desc->y_min, desc->y_max);
    }

    if (desc->wheel_size > 0)
    {
        uint32_t raw_wheel = mou_extract_bits(report_data, report_data_len,
                                              desc->wheel_offset, desc->wheel_size);
        delta_wheel = mou_scale_analog_signed(raw_wheel, desc->wheel_size,
                                              desc->wheel_min, desc->wheel_max);
    }

    if (desc->pan_size > 0)
    {
        uint32_t raw_pan = mou_extract_bits(report_data, report_data_len,
                                            desc->pan_offset, desc->pan_size);
        delta_pan = mou_scale_analog_signed(raw_pan, desc->pan_size,
                                            desc->pan_min, desc->pan_max);
    }

    // Update accumulated state
    mou_state.x += (int16_t)delta_x << 8;
    mou_state.y += (int16_t)delta_y << 8;
    mou_state.wheel += (int16_t)delta_wheel << 8;
    mou_state.pan += (int16_t)delta_pan << 8;

    // Update XRAM with new state
    mou_update_xram(buttons);
}

void mou_report_boot(uint8_t slot, void const *data, size_t size)
{
    (void)slot;

    /// Standard HID Boot Protocol Mouse Report.
    typedef struct TU_ATTR_PACKED
    {
        uint8_t buttons; /**< buttons mask for currently pressed buttons in the mouse. */
        int8_t x;        /**< Current delta x movement of the mouse. */
        int8_t y;        /**< Current delta y movement on the mouse. */
        int8_t wheel;    /**< Current delta wheel movement on the mouse. */
        int8_t pan;      // using AC Pan
    } hid_mouse_report_t;
    hid_mouse_report_t *mouse = (hid_mouse_report_t *)data;
    if (size >= 2)
        mou_state.x += (int16_t)mouse->x << 8;
    if (size >= 3)
        mou_state.y += (int16_t)mouse->y << 8;
    if (size >= 4)
        mou_state.wheel += (int16_t)mouse->wheel << 8;
    if (size >= 5)
        mou_state.pan += (int16_t)mouse->pan << 8;
    if (size >= 1)
        mou_update_xram(mouse->buttons);
}
