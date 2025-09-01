/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hid/hid.h"
#include "hid/mou.h"
#include "sys/mem.h"
#include <btstack_hid_parser.h>
#include <pico.h>
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
// Higher resolution x and y
uint16_t mou_x;
uint16_t mou_y;

static uint16_t mou_xram;

// Mouse descriptors are normalized to this structure.
typedef struct
{
    bool valid;
    int slot;          // HID protocol drivers use slots assigned in hid.h
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
} mou_connection_t;

static mou_connection_t mou_connections[MOU_MAX_MICE];

static mou_connection_t *find_connection_by_slot(int slot)
{
    for (int i = 0; i < MOU_MAX_MICE; ++i)
    {
        if (mou_connections[i].valid && mou_connections[i].slot == slot)
            return &mou_connections[i];
    }
    return NULL;
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

bool __in_flash("mou_mount") mou_mount(int slot, uint8_t const *desc_data, uint16_t desc_len)
{
    int desc_idx = -1;
    for (int i = 0; i < MOU_MAX_MICE; ++i)
        if (!mou_connections[i].valid)
            desc_idx = i;
    if (desc_idx < 0)
        return false;

    // Process raw HID descriptor into mou_connection_t
    mou_connection_t *conn = &mou_connections[desc_idx];
    memset(conn, 0, sizeof(mou_connection_t));
    for (int i = 0; i < 8; i++)
        conn->button_offsets[i] = 0xFFFF;
    conn->slot = slot;

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
                conn->x_offset = item.bit_pos;
                conn->x_size = item.size;
                conn->x_relative = (iterator.descriptor_item.item_value & 0x04) != 0;
                break;
            case 0x31: // Y axis
                conn->y_offset = item.bit_pos;
                conn->y_size = item.size;
                break;
            case 0x38: // Wheel
                conn->wheel_offset = item.bit_pos;
                conn->wheel_size = item.size;
                break;
            case 0x3C: // Pan/horizontal wheel
                conn->pan_offset = item.bit_pos;
                conn->pan_size = item.size;
                break;
            }
        }
        else if (item.usage_page == 0x09) // Button page
        {
            get_report_id = true;
            if (item.usage >= 1 && item.usage <= 8)
                conn->button_offsets[item.usage - 1] = item.bit_pos;
        }

        // Store report ID if this is the first one we encounter
        if (get_report_id && conn->report_id == 0 && item.report_id != 0xFFFF)
            conn->report_id = item.report_id;
    }

    // If it squeaks like a mouse.
    conn->valid = conn->x_relative && conn->x_size > 0;

    DBG("mou_mount: slot=%d, valid=%d, x_size=%d, y_size=%d\n",
        slot, conn->valid, conn->x_size, conn->y_size);

    return conn->valid;
}

bool mou_umount(int slot)
{
    mou_connection_t *conn = find_connection_by_slot(slot);
    if (conn == NULL)
        return false;
    conn->valid = false;
    return true;
}

void mou_report(int slot, void const *data, size_t size)
{
    mou_connection_t *conn = find_connection_by_slot(slot);
    if (conn == NULL)
        return;

    const uint8_t *report_data = (const uint8_t *)data;
    uint16_t report_data_len = size;

    if (conn->report_id != 0)
    {
        if (report_data_len == 0 || report_data[0] != conn->report_id)
            return;
        // Skip report ID byte
        report_data++;
        report_data_len--;
    }

    // Extract button states
    uint8_t buttons = 0;
    for (int i = 0; i < 8; i++)
    {
        if (conn->button_offsets[i] != 0xFFFF)
        {
            uint32_t button_val = hid_extract_bits(report_data, report_data_len,
                                                   conn->button_offsets[i], 1);
            if (button_val)
                buttons |= (1 << i);
        }
    }
    mou_state.buttons = buttons;

    // Extract movement data
    if (conn->x_size > 0)
        mou_x += hid_extract_signed(report_data, report_data_len,
                                    conn->x_offset, conn->x_size);
    mou_state.x = mou_x >> 1;
    if (conn->y_size > 0)
        mou_y += hid_extract_signed(report_data, report_data_len,
                                    conn->y_offset, conn->y_size);
    mou_state.y = mou_y >> 1;
    if (conn->wheel_size > 0)
        mou_state.wheel += hid_extract_signed(report_data, report_data_len,
                                              conn->wheel_offset, conn->wheel_size);
    if (conn->pan_size > 0)
        mou_state.pan += hid_extract_signed(report_data, report_data_len,
                                            conn->pan_offset, conn->pan_size);

    // Update XRAM with new state
    if (mou_xram != 0xFFFF)
        memcpy(&xram[mou_xram], &mou_state, sizeof(mou_state));
}
