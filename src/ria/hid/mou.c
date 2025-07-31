/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <pico.h>
#include "btstack_hid_parser.h"
#include "tusb_config.h"
#include "hid/des.h"
#include "hid/mou.h"
#include "sys/mem.h"
#include <string.h>

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_MOU)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define MOU_MAX_MICE 4

// This is the report we generate for XRAM.
static struct
{
    uint8_t buttons;
    uint8_t x;
    uint8_t y;
    uint8_t wheel;
    uint8_t pan;
} mou_state;

static uint16_t mou_xram = 0xFFFF;

// Mouse descriptors are normalized to this structure.
typedef struct
{
    bool valid;
    uint8_t slot;      // HID protocol drivers use slots assigned in hid.h
    uint8_t report_id; // If non zero, the first report byte must match and will be skipped
    uint16_t button_offsets[8];
    bool x_relative;   // Will be true for mice
    uint16_t x_offset; // X axis
    uint8_t x_size;
    uint16_t y_offset; // Y axis
    uint8_t y_size;
    uint16_t wheel_offset; // Wheel/scroll wheel
    uint8_t wheel_size;
    uint16_t pan_offset; // Horizontal pan/tilt
    uint8_t pan_size;
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
    if (word != 0xFFFF && word > 0x10000 - sizeof(mou_state))
        return false;
    mou_xram = word;
    if (mou_xram != 0xFFFF)
        memcpy(&xram[mou_xram], &mou_state, sizeof(mou_state));
    return true;
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

        // Log each HID usage item
        // DBG("HID item: page=0x%02x, usage=0x%02x, report_id=0x%04x\n",
        //     item.usage_page, item.usage, item.report_id);

        bool get_report_id = false;
        if (item.usage_page == 0x01) // Generic Desktop
        {
            get_report_id = true;
            switch (item.usage)
            {
            case 0x30: // X axis
                desc->x_offset = item.bit_pos;
                desc->x_size = item.size;
                desc->x_relative = (iterator.descriptor_item.item_value & 0x04) != 0;
                break;
            case 0x31: // Y axis
                desc->y_offset = item.bit_pos;
                desc->y_size = item.size;
                break;
            case 0x38: // Wheel
                desc->wheel_offset = item.bit_pos;
                desc->wheel_size = item.size;
                break;
            case 0x3C: // Pan/horizontal wheel
                desc->pan_offset = item.bit_pos;
                desc->pan_size = item.size;
                break;
            }
        }
        else if (item.usage_page == 0x09) // Button page
        {
            get_report_id = true;
            if (item.usage >= 1 && item.usage <= 8)
                desc->button_offsets[item.usage - 1] = item.bit_pos;
        }

        // Store report ID if this is the first one we encounter
        if (get_report_id && desc->report_id == 0 && item.report_id != 0xFFFF)
            desc->report_id = item.report_id;
    }

    // If it squeaks like a mouse.
    desc->valid = desc->x_relative && desc->x_size > 0;

    // Debug print parsed descriptor
    DBG("mou_parse_descriptor: report_id=%d, valid=%d\n", desc->report_id, desc->valid);
    DBG("  X: offset=%d, size=%d, relative=%d\n", desc->x_offset, desc->x_size, desc->x_relative);
    DBG("  Y: offset=%d, size=%d\n", desc->y_offset, desc->y_size);
    DBG("  Wheel: offset=%d, size=%d\n", desc->wheel_offset, desc->wheel_size);
    DBG("  Pan: offset=%d, size=%d\n", desc->pan_offset, desc->pan_size);
    DBG("  Buttons: [%d,%d,%d,%d,%d,%d,%d,%d]\n",
        desc->button_offsets[0], desc->button_offsets[1], desc->button_offsets[2], desc->button_offsets[3],
        desc->button_offsets[4], desc->button_offsets[5], desc->button_offsets[6], desc->button_offsets[7]);
}

bool __in_flash("mou_mount") mou_mount(uint8_t slot, uint8_t const *desc_data, uint16_t desc_len)
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

    if (desc->report_id != 0)
    {
        if (report_data_len == 0 || report_data[0] != desc->report_id)
            return;
        // Skip report ID byte
        report_data++;
        report_data_len--;
    }

    // Extract button states
    uint8_t buttons = 0;
    for (int i = 0; i < 8; i++)
    {
        if (desc->button_offsets[i] != 0xFFFF)
        {
            uint32_t button_val = des_extract_bits(report_data, report_data_len,
                                                   desc->button_offsets[i], 1);
            if (button_val)
                buttons |= (1 << i);
        }
    }
    mou_state.buttons = buttons;

    // Extract movement data
    if (desc->x_size > 0)
        mou_state.x += des_extract_signed(report_data, report_data_len,
                                          desc->x_offset, desc->x_size);
    if (desc->y_size > 0)
        mou_state.y += des_extract_signed(report_data, report_data_len,
                                          desc->y_offset, desc->y_size);
    if (desc->wheel_size > 0)
        mou_state.wheel += des_extract_signed(report_data, report_data_len,
                                              desc->wheel_offset, desc->wheel_size);
    if (desc->pan_size > 0)
        mou_state.pan += des_extract_signed(report_data, report_data_len,
                                            desc->pan_offset, desc->pan_size);

    // Update XRAM with new state
    if (mou_xram != 0xFFFF)
        memcpy(&xram[mou_xram], &mou_state, sizeof(mou_state));
}

void mou_report_boot(uint8_t slot, void const *data, size_t size)
{
    (void)slot;
    typedef struct
    {
        uint8_t buttons;
        int8_t x;
        int8_t y;
        int8_t wheel;
        int8_t pan;
    } mou_hid_boot_t;
    mou_hid_boot_t *mouse = (mou_hid_boot_t *)data;
    if (size >= 1)
        mou_state.buttons += mouse->buttons;
    if (size >= 2)
        mou_state.x += mouse->x;
    if (size >= 3)
        mou_state.y += mouse->y;
    if (size >= 4)
        mou_state.wheel += mouse->wheel;
    if (size >= 5)
        mou_state.pan += mouse->pan;
    if (mou_xram != 0xFFFF)
        memcpy(&xram[mou_xram], &mou_state, sizeof(mou_state));
}
