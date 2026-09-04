/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/hid.h"
#include "core/hid/mouse.h"
#include "core/sys/xram.h"
#include "machine.h"
#include <string.h>

#if defined(DEBUG_HID) || defined(DEBUG_HID_MOUSE)
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
} mouse_state;
// Higher resolution x and y
static uint16_t mouse_x;
static uint16_t mouse_y;

static uint16_t mouse_xram;


static mouse_connection_t mouse_connections[MOUSE_MAX_MICE];

static mouse_connection_t *mouse_get_connection_by_slot(int slot)
{
    for (int i = 0; i < MOUSE_MAX_MICE; ++i)
    {
        if (mouse_connections[i].valid && mouse_connections[i].slot == slot)
            return &mouse_connections[i];
    }
    return NULL;
}

void HOST_IN_FLASH("mouse_init") mouse_init(void)
{
    mouse_stop();
}

void mouse_stop(void)
{
    mouse_xram = 0xFFFF;
}

static void mouse_write_xram(void)
{
    if (mouse_xram != 0xFFFF)
        memcpy((uint8_t *)&xram[mouse_xram], &mouse_state, sizeof(mouse_state));
}

bool mouse_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - sizeof(mouse_state))
        return false;
    mouse_xram = word;
    mouse_write_xram();
    return true;
}

bool HOST_IN_FLASH("mouse_mount") mouse_mount(int slot, const mouse_connection_t *desc)
{
    if (!desc->valid)
        return false;
    for (int i = 0; i < MOUSE_MAX_MICE; ++i)
    {
        if (mouse_connections[i].valid)
            continue;
        mouse_connections[i] = *desc;
        mouse_connections[i].slot = slot;
        DBG("mouse_mount: slot=%d, report_id=%d, x=%d/%d rel=%d\n", slot,
            desc->report_id, desc->x_offset, desc->x_size, desc->x_relative);
        return true;
    }
    return false;
}

bool mouse_umount(int slot)
{
    mouse_connection_t *conn = mouse_get_connection_by_slot(slot);
    if (conn == NULL)
        return false;
    DBG("mouse_umount: slot=%d, valid=%d, report_id=%d\n", slot, conn->valid, conn->report_id);
    conn->valid = false;
    uint8_t merged = 0;
    for (int i = 0; i < MOUSE_MAX_MICE; ++i)
        if (mouse_connections[i].valid)
            merged |= mouse_connections[i].buttons;
    mouse_state.buttons = merged;
    if (mouse_xram != 0xFFFF)
        memcpy((uint8_t *)&xram[mouse_xram], &mouse_state, sizeof(mouse_state));
    return true;
}

void mouse_report(int slot, uint8_t const *data, size_t size)
{
    mouse_connection_t *conn = mouse_get_connection_by_slot(slot);
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
    for (int i = 0; i < MOUSE_MAX_MICE; ++i)
        if (mouse_connections[i].valid)
            merged |= mouse_connections[i].buttons;
    mouse_state.buttons = merged;

    // Extract movement data
    mouse_x += hid_extract_signed(report_data, report_data_len,
                                  conn->x_offset, conn->x_size);
    mouse_state.x = mouse_x >> 1;
    if (conn->y_size > 0)
        mouse_y += hid_extract_signed(report_data, report_data_len,
                                      conn->y_offset, conn->y_size);
    mouse_state.y = mouse_y >> 1;
    if (conn->wheel_size > 0)
        mouse_state.wheel += hid_extract_signed(report_data, report_data_len,
                                                conn->wheel_offset, conn->wheel_size);
    if (conn->pan_size > 0)
        mouse_state.pan += hid_extract_signed(report_data, report_data_len,
                                              conn->pan_offset, conn->pan_size);

    mouse_write_xram();
}

bool mouse_is_mapped(void)
{
    return mouse_xram != 0xFFFF;
}

/* A host whose OS decodes its own pointer has no report to hand over,
 * so it moves the same counters a report would have. The block carries
 * half of what a mouse counts, so a host count -- which is already in
 * the block's units -- is doubled on the way in and arrives whole. */
static float mouse_acc_x, mouse_acc_y;

void mouse_host_move(float dx, float dy)
{
    mouse_acc_x += dx;
    mouse_acc_y += dy;
    int ix = (int)mouse_acc_x; // truncate toward zero; keep the remainder
    int iy = (int)mouse_acc_y;
    if (ix == 0 && iy == 0)
        return;
    mouse_acc_x -= ix;
    mouse_acc_y -= iy;
    mouse_x += (uint16_t)(ix * 2);
    mouse_y += (uint16_t)(iy * 2);
    mouse_state.x = mouse_x >> 1;
    mouse_state.y = mouse_y >> 1;
    mouse_write_xram();
}

void mouse_host_wheel(int dwheel, int dpan)
{
    if (dwheel == 0 && dpan == 0)
        return;
    mouse_state.wheel += (uint8_t)(int8_t)dwheel;
    mouse_state.pan += (uint8_t)(int8_t)dpan;
    mouse_write_xram();
}

void mouse_host_buttons(uint8_t buttons)
{
    if (buttons == mouse_state.buttons)
        return;
    mouse_state.buttons = buttons;
    mouse_write_xram();
}
