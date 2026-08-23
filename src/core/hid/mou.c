/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/hid.h"
#include "core/hid/mou.h"
#include "core/mem.h"
#include <pico.h>
#include <string.h>

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_MOU)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
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
static uint16_t mou_x;
static uint16_t mou_y;

static uint16_t mou_xram;

// Mouse descriptors are normalized to this structure.
typedef struct
{
    bool valid;
    int slot;          // HID slot
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
    uint8_t buttons; // Last reported button state
} mou_connection_t;

static mou_connection_t mou_connections[MOU_MAX_MICE];

static mou_connection_t *mou_get_connection_by_slot(int slot)
{
    for (int i = 0; i < MOU_MAX_MICE; ++i)
    {
        if (mou_connections[i].valid && mou_connections[i].slot == slot)
            return &mou_connections[i];
    }
    return NULL;
}

void __in_flash("mou_init") mou_init(void)
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
        memcpy((uint8_t *)&xram[mou_xram], &mou_state, sizeof(mou_state));
    return true;
}

bool __in_flash("mou_mount") mou_mount(int slot, const hid_report_map_t *map)
{
    int conn_num = -1;
    for (int i = 0; i < MOU_MAX_MICE; ++i)
        if (!mou_connections[i].valid)
        {
            conn_num = i;
            break;
        }
    if (conn_num < 0)
        return false;

    mou_connection_t *conn = &mou_connections[conn_num];
    memset(conn, 0, sizeof(mou_connection_t));
    conn->slot = slot;
    conn->report_id = map->report_id;
    for (int i = 0; i < 8; i++)
        conn->button_offsets[i] = map->button_bit[i];
    conn->x_offset = map->axis[HID_AXIS_X].bit_pos;
    conn->x_size = map->axis[HID_AXIS_X].size;
    conn->x_relative = map->axis[HID_AXIS_X].relative;
    conn->y_offset = map->axis[HID_AXIS_Y].bit_pos;
    conn->y_size = map->axis[HID_AXIS_Y].size;
    conn->wheel_offset = map->axis[HID_AXIS_WHEEL].bit_pos;
    conn->wheel_size = map->axis[HID_AXIS_WHEEL].size;
    conn->pan_offset = map->axis[HID_AXIS_PAN].bit_pos;
    conn->pan_size = map->axis[HID_AXIS_PAN].size;

    // If it squeaks like a mouse.
    conn->valid = conn->x_relative && conn->x_size > 0;

    DBG("mou_mount: slot=%d, valid=%d, report_id=%d\n", slot, conn->valid, conn->report_id);
    DBG("  x: offset=%d, size=%d, relative=%d\n", conn->x_offset, conn->x_size, conn->x_relative);
    DBG("  y: offset=%d, size=%d\n", conn->y_offset, conn->y_size);
    DBG("  wheel: offset=%d, size=%d\n", conn->wheel_offset, conn->wheel_size);
    DBG("  pan: offset=%d, size=%d\n", conn->pan_offset, conn->pan_size);

    return conn->valid;
}

bool mou_umount(int slot)
{
    mou_connection_t *conn = mou_get_connection_by_slot(slot);
    if (conn == NULL)
        return false;
    DBG("mou_umount: slot=%d, valid=%d, report_id=%d\n", slot, conn->valid, conn->report_id);
    conn->valid = false;
    uint8_t merged = 0;
    for (int i = 0; i < MOU_MAX_MICE; ++i)
        if (mou_connections[i].valid)
            merged |= mou_connections[i].buttons;
    mou_state.buttons = merged;
    if (mou_xram != 0xFFFF)
        memcpy((uint8_t *)&xram[mou_xram], &mou_state, sizeof(mou_state));
    return true;
}

void mou_report(int slot, uint8_t const *data, size_t size)
{
    mou_connection_t *conn = mou_get_connection_by_slot(slot);
    if (conn == NULL)
        return;

    const uint8_t *report_data = data;
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
    conn->buttons = buttons;
    uint8_t merged = 0;
    for (int i = 0; i < MOU_MAX_MICE; ++i)
        if (mou_connections[i].valid)
            merged |= mou_connections[i].buttons;
    mou_state.buttons = merged;

    // Extract movement data
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
        memcpy((uint8_t *)&xram[mou_xram], &mou_state, sizeof(mou_state));
}
