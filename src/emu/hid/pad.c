/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/hid/pad.h"
#include "emu/sys/mem.h"
#include <string.h>

#define PAD_PLAYERS 4
#define PAD_STRIDE 10 /* sizeof(firmware pad_xram_t) */

/* Byte offsets within a player record (mirror pad_xram_t). */
enum
{
    PAD_OFF_DPAD = 0,    /* dpad dirs (0x0F) + sony (0x40) + connected (0x80) */
    PAD_OFF_STICKS = 1,  /* left (0x0F) and right (0xF0) digital stick dirs */
    PAD_OFF_BUTTON0 = 2, /* A/B/C/X/Y/Z + L1/R1 */
    PAD_OFF_BUTTON1 = 3, /* L2/R2 + select/start/home + L3/R3 */
    PAD_OFF_LX = 4, PAD_OFF_LY = 5, PAD_OFF_RX = 6, PAD_OFF_RY = 7,
    PAD_OFF_LT = 8, PAD_OFF_RT = 9,
};

#define PAD_CONNECTED 0x80
#define PAD_SONY 0x40
#define PAD_DEADZONE 32 /* analog->digital threshold (mirror firmware pad.c) */

static uint8_t pad_state[PAD_PLAYERS][PAD_STRIDE];
static uint16_t pad_xram = 0xFFFF; /* 0xFFFF = not mapped */

/* Analog stick -> 4-bit hat direction (up/down/left/right = 1/2/4/8), the same
 * deadzone + 2:1 cardinal/diagonal split the firmware uses so the digital
 * sticks byte matches real hardware. */
static uint8_t pad_encode_stick(int8_t x, int8_t y)
{
    if (x >= -PAD_DEADZONE && x <= PAD_DEADZONE &&
        y >= -PAD_DEADZONE && y <= PAD_DEADZONE)
        return 0;
    int16_t ax = (x < 0) ? -x : x;
    int16_t ay = (y < 0) ? -y : y;
    if (ay >= ax * 2)
        return (y < 0) ? 1 : 2;
    if (ax >= ay * 2)
        return (x < 0) ? 4 : 8;
    return (uint8_t)((y < 0 ? 1 : 2) | (x < 0 ? 4 : 8));
}

static void pad_write_xram(void)
{
    if (pad_xram != 0xFFFF)
        memcpy(&xram[pad_xram], pad_state, sizeof(pad_state));
}

/* Resolve a flat button id to its (byte offset, bit mask) in a player record. */
static bool pad_button_loc(pad_button_t button, int *off, uint8_t *mask)
{
    switch (button)
    {
    case PAD_BTN_DPAD_UP:    *off = PAD_OFF_DPAD;    *mask = 0x01; return true;
    case PAD_BTN_DPAD_DOWN:  *off = PAD_OFF_DPAD;    *mask = 0x02; return true;
    case PAD_BTN_DPAD_LEFT:  *off = PAD_OFF_DPAD;    *mask = 0x04; return true;
    case PAD_BTN_DPAD_RIGHT: *off = PAD_OFF_DPAD;    *mask = 0x08; return true;
    case PAD_BTN_A:          *off = PAD_OFF_BUTTON0; *mask = 0x01; return true;
    case PAD_BTN_B:          *off = PAD_OFF_BUTTON0; *mask = 0x02; return true;
    case PAD_BTN_X:          *off = PAD_OFF_BUTTON0; *mask = 0x08; return true;
    case PAD_BTN_Y:          *off = PAD_OFF_BUTTON0; *mask = 0x10; return true;
    case PAD_BTN_L1:         *off = PAD_OFF_BUTTON0; *mask = 0x40; return true;
    case PAD_BTN_R1:         *off = PAD_OFF_BUTTON0; *mask = 0x80; return true;
    case PAD_BTN_L2:         *off = PAD_OFF_BUTTON1; *mask = 0x01; return true;
    case PAD_BTN_R2:         *off = PAD_OFF_BUTTON1; *mask = 0x02; return true;
    case PAD_BTN_SELECT:     *off = PAD_OFF_BUTTON1; *mask = 0x04; return true;
    case PAD_BTN_START:      *off = PAD_OFF_BUTTON1; *mask = 0x08; return true;
    case PAD_BTN_HOME:       *off = PAD_OFF_BUTTON1; *mask = 0x10; return true;
    case PAD_BTN_L3:         *off = PAD_OFF_BUTTON1; *mask = 0x20; return true;
    case PAD_BTN_R3:         *off = PAD_OFF_BUTTON1; *mask = 0x40; return true;
    }
    return false;
}

bool pad_set_xram(uint16_t addr)
{
    if (addr != 0xFFFF && addr > 0x10000 - sizeof(pad_state))
        return false;
    pad_xram = addr;
    pad_write_xram(); /* publish current (default-disconnected) state */
    return true;
}

void pad_connect(int player, bool connected)
{
    if (player < 0 || player >= PAD_PLAYERS)
        return;
    if (connected)
        pad_state[player][PAD_OFF_DPAD] |= PAD_CONNECTED;
    else
        memset(pad_state[player], 0, PAD_STRIDE); /* blank record = unplugged */
    pad_write_xram();
}

void pad_hid_set(int player, pad_button_t button, bool down)
{
    int off;
    uint8_t mask;
    if (player < 0 || player >= PAD_PLAYERS || !pad_button_loc(button, &off, &mask))
        return;
    if (down)
        pad_state[player][off] |= mask;
    else
        pad_state[player][off] &= (uint8_t)~mask;
    pad_write_xram();
}

void pad_stop(void)
{
    memset(pad_state, 0, sizeof(pad_state));
    pad_xram = 0xFFFF;
}

bool pad_is_mapped(void)
{
    return pad_xram != 0xFFFF;
}

void pad_host_report(int player, uint8_t dpad, uint8_t button0, uint8_t button1,
                     int lx, int ly, int rx, int ry, int lt, int rt, bool sony)
{
    if (player < 0 || player >= PAD_PLAYERS)
        return;
    /* Couple the analog triggers to the L2/R2 buttons both ways, as the firmware
     * does: a digital press with no analog reads full-scale, and past-deadzone
     * analog asserts the button — so a program sees the pair consistently. */
    if ((button1 & 0x01) && lt == 0)
        lt = 255;
    if ((button1 & 0x02) && rt == 0)
        rt = 255;
    if (lt > PAD_DEADZONE)
        button1 |= 0x01;
    if (rt > PAD_DEADZONE)
        button1 |= 0x02;

    uint8_t *r = pad_state[player];
    r[PAD_OFF_DPAD] = (uint8_t)(dpad | PAD_CONNECTED | (sony ? PAD_SONY : 0));
    r[PAD_OFF_STICKS] = (uint8_t)(pad_encode_stick((int8_t)lx, (int8_t)ly) |
                                  (pad_encode_stick((int8_t)rx, (int8_t)ry) << 4));
    r[PAD_OFF_BUTTON0] = button0;
    r[PAD_OFF_BUTTON1] = button1;
    r[PAD_OFF_LX] = (uint8_t)(int8_t)lx;
    r[PAD_OFF_LY] = (uint8_t)(int8_t)ly;
    r[PAD_OFF_RX] = (uint8_t)(int8_t)rx;
    r[PAD_OFF_RY] = (uint8_t)(int8_t)ry;
    r[PAD_OFF_LT] = (uint8_t)lt;
    r[PAD_OFF_RT] = (uint8_t)rt;
    pad_write_xram();
}
