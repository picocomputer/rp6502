/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb.h"
#include "hid/mou.h"
#include "sys/mem.h"

static struct
{
    uint8_t buttons;
    uint8_t x;
    uint8_t y;
    uint8_t wheel;
    uint8_t pan;
} mou_xram_data;
static uint16_t mou_xram = 0xFFFF;

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
    if (word != 0xFFFF && word > 0x10000 - sizeof(mou_xram_data))
        return false;
    mou_xram = word;
    return true;
}

void mou_report(uint8_t idx, void const *report, size_t size)
{
    (void)idx;
    hid_mouse_report_t const *mouse = report;
    if (size >= 1)
        mou_xram_data.buttons = mouse->buttons;
    if (size >= 2)
        mou_xram_data.x += mouse->x;
    if (size >= 3)
        mou_xram_data.y += mouse->y;
    if (size >= 4)
        mou_xram_data.wheel += mouse->wheel;
    if (size >= 5)
        mou_xram_data.pan += mouse->pan;
    if (mou_xram != 0xFFFF)
        memcpy(&xram[mou_xram], &mou_xram_data, sizeof(mou_xram_data));
}
