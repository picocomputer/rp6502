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

static void mou_write_xram(void)
{
    if (mou_xram != 0xFFFF)
        memcpy((uint8_t *)&xram[mou_xram], &mou_state, sizeof(mou_state));
}

bool mou_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - sizeof(mou_state))
        return false;
    mou_xram = word;
    mou_write_xram();
    return true;
}

bool __in_flash("mou_mount") mou_mount(int slot, const mou_connection_t *desc)
{
    if (!desc->valid)
        return false;
    for (int i = 0; i < MOU_MAX_MICE; ++i)
    {
        if (mou_connections[i].valid)
            continue;
        mou_connections[i] = *desc;
        mou_connections[i].slot = slot;
        DBG("mou_mount: slot=%d, report_id=%d, x=%d/%d rel=%d\n", slot,
            desc->report_id, desc->x_offset, desc->x_size, desc->x_relative);
        return true;
    }
    return false;
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

    mou_write_xram();
}

bool mou_is_mapped(void)
{
    return mou_xram != 0xFFFF;
}

/* A host whose OS decodes its own pointer has no report to hand over,
 * so it moves the same counters a report would have. The block carries
 * half of what a mouse counts, so a host count -- which is already in
 * the block's units -- is doubled on the way in and arrives whole. */
static float mou_acc_x, mou_acc_y;

void mou_host_move(float dx, float dy)
{
    mou_acc_x += dx;
    mou_acc_y += dy;
    int ix = (int)mou_acc_x; // truncate toward zero; keep the remainder
    int iy = (int)mou_acc_y;
    if (ix == 0 && iy == 0)
        return;
    mou_acc_x -= ix;
    mou_acc_y -= iy;
    mou_x += (uint16_t)(ix * 2);
    mou_y += (uint16_t)(iy * 2);
    mou_state.x = mou_x >> 1;
    mou_state.y = mou_y >> 1;
    mou_write_xram();
}

void mou_host_wheel(int dwheel, int dpan)
{
    if (dwheel == 0 && dpan == 0)
        return;
    mou_state.wheel += (uint8_t)(int8_t)dwheel;
    mou_state.pan += (uint8_t)(int8_t)dpan;
    mou_write_xram();
}

void mou_host_buttons(uint8_t buttons)
{
    if (buttons == mou_state.buttons)
        return;
    mou_state.buttons = buttons;
    mou_write_xram();
}
