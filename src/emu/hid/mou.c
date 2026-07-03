/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/hid/mou.h"
#include "emu/sys/mem.h"
#include <string.h>

#define MOU_STRIDE 5 /* sizeof the firmware {buttons,x,y,wheel,pan} report */

enum
{
    MOU_OFF_BUTTONS = 0, /* bit 0 left, 1 right, 2 middle (HID button order) */
    MOU_OFF_X = 1,
    MOU_OFF_Y = 2,
    MOU_OFF_WHEEL = 3,
    MOU_OFF_PAN = 4,
};

static uint8_t mou_state[MOU_STRIDE];
static uint16_t mou_xram = 0xFFFF; /* 0xFFFF = not mapped */
static float mou_acc_x, mou_acc_y; /* sub-count motion carried between moves */

static void mou_write_xram(void)
{
    if (mou_xram != 0xFFFF)
        memcpy(&xram[mou_xram], mou_state, sizeof(mou_state));
}

bool mou_set_xram(uint16_t addr)
{
    if (addr != 0xFFFF && addr > 0x10000 - sizeof(mou_state))
        return false;
    mou_xram = addr;
    mou_write_xram();
    return true;
}

bool mou_is_mapped(void)
{
    return mou_xram != 0xFFFF;
}

void mou_host_move(float dx, float dy)
{
    mou_acc_x += dx;
    mou_acc_y += dy;
    int ix = (int)mou_acc_x; /* truncate toward zero; keep the remainder */
    int iy = (int)mou_acc_y;
    if (ix == 0 && iy == 0)
        return;
    mou_acc_x -= ix;
    mou_acc_y -= iy;
    mou_state[MOU_OFF_X] = (uint8_t)(mou_state[MOU_OFF_X] + ix);
    mou_state[MOU_OFF_Y] = (uint8_t)(mou_state[MOU_OFF_Y] + iy);
    mou_write_xram();
}

void mou_host_wheel(int dwheel, int dpan)
{
    if (dwheel == 0 && dpan == 0)
        return;
    mou_state[MOU_OFF_WHEEL] = (uint8_t)(mou_state[MOU_OFF_WHEEL] + dwheel);
    mou_state[MOU_OFF_PAN] = (uint8_t)(mou_state[MOU_OFF_PAN] + dpan);
    mou_write_xram();
}

void mou_host_buttons(uint8_t buttons)
{
    if (buttons == mou_state[MOU_OFF_BUTTONS])
        return;
    mou_state[MOU_OFF_BUTTONS] = buttons;
    mou_write_xram();
}

void mou_reset(void)
{
    memset(mou_state, 0, sizeof(mou_state));
    mou_acc_x = mou_acc_y = 0;
    mou_xram = 0xFFFF;
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* The web shell shows the "click to capture" hint only once a program maps the
 * mouse. The capture itself (pointer lock) and motion scaling are handled by the
 * shared app_sokol mouse path — sokol implements pointer lock on the web too. */
EMSCRIPTEN_KEEPALIVE int emu_mou_mapped(void)
{
    return mou_is_mapped() ? 1 : 0;
}
#endif
