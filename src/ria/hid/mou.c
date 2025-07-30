/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb.h"
#include "hid/mou.h"
#include "sys/mem.h"

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
    // TODO examples follow, undecided what goes here
    uint16_t x_offset; // X
    uint8_t x_size;
    int32_t x_min;
    int32_t x_max;
    uint16_t y_offset; // Y
    uint8_t y_size;
    int32_t y_min;
    int32_t y_max;
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

bool mou_mount(uint8_t slot, uint8_t const *desc_data, uint16_t desc_len)
{
    int desc_idx = -1;
    for (int desc_idx = 0; desc_idx < MOU_MAX_MICE; ++desc_idx)
        if (!mou_descriptors[desc_idx].valid)
            break;
    if (desc_idx < 0)
        return false;
    mou_descriptor_t *desc = &mou_descriptors[desc_idx];

    // TODO process raw HID descriptor into desc

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

    // TODO process a standard report
    // mou_update_xram(buttons);
}

void mou_report_boot(uint8_t slot, void const *data, size_t size)
{
    (void)slot;
    hid_mouse_report_t const *mouse = data;
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
