/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/hid.h"
#include "core/hid/mouse.h"
#include "core/sys/debug_log.h"
#include "core/sys/xram.h"
#include "machine.h"
#include <string.h>

static struct
{
    uint8_t buttons;
    uint8_t x;
    uint8_t y;
    uint8_t wheel;
    uint8_t pan;
} mouse_state;
/* Raw device counts. The block carries these shifted down by one, so it
 * advances one unit for every two counts the mouse reports. */
static uint16_t mouse_x;
static uint16_t mouse_y;

/* The fraction of a count a host's motion leaves over, in Q16.16. */
static int32_t mouse_acc_x, mouse_acc_y;

static uint16_t mouse_xram;

static mouse_connection_t mouse_connections[MOUSE_MAX_MICE];

/* An address that would put the block past the end of XRAM would have the
 * next write of it go past it. */
static bool mouse_xram_check(sst_cursor_t *c)
{
    if (mouse_xram > 0x10000 - sizeof(mouse_state) && mouse_xram != 0xFFFF)
        return sst_refuse(c, "mouse block past the end of XRAM");
    return true;
}

#define MOUSE_STATE_ROWS(Y, s)                              \
    Y(U8, s.buttons) Y(U8, s.x) Y(U8, s.y)                  \
    Y(U8, s.wheel) Y(U8, s.pan)
#define MOUSE_SST(X)                                        \
    X(U16, mouse_xram)                                      \
    X(STRUCT, mouse_state, MOUSE_STATE_ROWS)                \
    X(U16, mouse_x)                                         \
    X(U16, mouse_y)                                         \
    X(I32, mouse_acc_x)                                     \
    X(I32, mouse_acc_y)                                     \
    X(VALIDATE, mouse_xram_check)                           \
    X(SKIP, mouse_connections, "nothing mounts on a machine that saves")
SST_TABLE(mouse, MOUSE_SST)

static mouse_connection_t *HOST_IN_RAM_FUNC mouse_get_connection_by_slot(int slot)
{
    for (int i = 0; i < MOUSE_MAX_MICE; ++i)
    {
        if (mouse_connections[i].valid && mouse_connections[i].slot == slot)
            return &mouse_connections[i];
    }
    return NULL;
}

void HOST_IN_FLASH_FUNC mouse_init(void)
{
    mouse_stop();
}

void mouse_stop(void)
{
    mouse_xram = 0xFFFF;
}

static void HOST_IN_RAM_FUNC mouse_write_xram(void)
{
    if (mouse_xram != 0xFFFF)
        memcpy((uint8_t *)&xram[mouse_xram], &mouse_state, sizeof(mouse_state));
}

bool HOST_IN_FLASH_FUNC mouse_xreg(uint16_t word)
{
    bool mapped = word <= 0x10000 - sizeof(mouse_state);
    mouse_xram = mapped ? word : 0xFFFF;
    mouse_write_xram();
    return mapped || word == 0xFFFF;
}

bool mouse_mount(int slot, const mouse_connection_t *desc)
{
    if (!desc->valid)
        return false;
    for (int i = 0; i < MOUSE_MAX_MICE; ++i)
    {
        if (mouse_connections[i].valid)
            continue;
        mouse_connections[i] = *desc;
        mouse_connections[i].slot = slot;
        RP6502_LOG(hid, INFO, "mouse mount slot=%d, report_id=%d, x=%d/%d rel=%d", slot,
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
    RP6502_LOG(hid, INFO, "mouse umount slot=%d, valid=%d, report_id=%d", slot, conn->valid, conn->report_id);
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

void HOST_IN_RAM_FUNC mouse_report(int slot, uint8_t const *data, size_t size)
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
        report_data++;
        report_data_len--;
    }

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

/* A host count is already in the block's units, and the counters behind the
 * block run at twice that, so host motion is doubled on the way in. */
void mouse_host_move(int32_t dx, int32_t dy)
{
    mouse_acc_x += dx;
    mouse_acc_y += dy;
    int ix = mouse_acc_x / MOUSE_ONE;
    int iy = mouse_acc_y / MOUSE_ONE;
    if (ix == 0 && iy == 0)
        return;
    mouse_acc_x -= ix * MOUSE_ONE;
    mouse_acc_y -= iy * MOUSE_ONE;
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
