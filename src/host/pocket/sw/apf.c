/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "apf.h"
#include "core/sys/sst.h"
#include "mmio.h"

#include "core/hid/hid.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/sys/event.h"
#include "core/hid/usage.h"

#include <stdint.h>
#include <string.h>

#define APF_TYPE_NONE 0
#define APF_TYPE_POCKET 1
#define APF_TYPE_PAD 2
#define APF_TYPE_PAD_ANA 3
#define APF_TYPE_KEYBOARD 4
#define APF_TYPE_MOUSE 5

/* Which bit of a controller's key register each button is, in the order of
 * the buttons' bitmap (gamepad_host_t), 0xFF for one it has none of. The
 * d-pad is its four low bits, up, down, left and right. */
static const uint8_t apf_button_bits[16] = {
    4, 5, 0xFF, 6, 7, 0xFF, 8, 9,
    10, 11, 14, 15, 0xFF, 12, 13, 0xFF};

static const keyboard_connection_t apf_keyboard_desc = {
    .valid = true,
    .runs = {{.bit_pos = 0, .usage_min = HID_KEY_CONTROL_LEFT, .count = 8}},
    .codes_offset = 2 * 8,
    .codes_count = 6};

static const mouse_connection_t apf_mouse_desc = {
    .valid = true,
    .button_offsets = {0, 1, 2, 3, 4, 5, 6, 7},
    .x_relative = true,
    .x_offset = 2 * 8, .x_size = 16,
    .y_offset = 4 * 8, .y_size = 16};

/* The keyboard's modifier field and the mouse's movement fields are little
 * endian, and each is packed into its register with its first byte in the
 * more significant position, so such a field's low byte sits in the high
 * half of its halfword. */
static inline uint16_t apf_swap16(uint32_t word)
{
    return (uint16_t)(((word & 0xFFu) << 8) | ((word >> 8) & 0xFFu));
}

#define APF_REPORT_MAX 8

static struct
{
    uint8_t type;
    int8_t slot;
    uint8_t report[APF_REPORT_MAX];
    uint8_t len;
    uint16_t mouse_seen;
    bool mouse_have_seen;
} apf_slots[MMIO_CONT_SLOTS];

/* The controllers as they are, each port its player, and as they were last
 * told, which apf_refresh forgets so that they are told again. */
static event_gamepads_t apf_pads, apf_pads_told;
static bool apf_pads_stale;

#define APF_SST(X)                                                              \
    X(SKIP, apf_slots, "the host's controllers, which it reports again")       \
    X(SKIP, apf_pads, "the host's controllers, which it reports again")        \
    X(SKIP, apf_pads_told, "the host's controllers, which it reports again")   \
    X(SKIP, apf_pads_stale, "set by apf_refresh, after a load")
SST_TABLE(apf, APF_SST)

/* A keyboard and a mouse go to their USB drivers, as a USB one would. A
 * controller is no USB device: its port is its player (apf_pad). */
static void apf_mount(int slot, uint8_t type)
{
    switch (type)
    {
    case APF_TYPE_KEYBOARD:
        apf_slots[slot].slot = (int8_t)hid_mount(&apf_keyboard_desc, NULL, NULL, NULL, 0, 0, 0);
        return;
    case APF_TYPE_MOUSE:
        apf_slots[slot].slot = (int8_t)hid_mount(NULL, &apf_mouse_desc, NULL, NULL, 0, 0, 0);
        return;
    default:
        return;
    }
}

static void apf_umount(int slot)
{
    if (apf_slots[slot].slot >= 0)
        hid_umount(apf_slots[slot].slot);
    apf_slots[slot].slot = -1;
}

static void apf_report(int slot, const uint8_t *report, uint8_t len)
{
    hid_report(apf_slots[slot].slot, report, len);
}

static bool apf_changed(int slot, const uint8_t *report, uint8_t len)
{
    if (apf_slots[slot].len == len &&
        !memcmp(apf_slots[slot].report, report, len))
        return false;
    memcpy(apf_slots[slot].report, report, len);
    apf_slots[slot].len = len;
    return true;
}

static uint8_t apf_build(uint8_t type, uint32_t key, uint32_t joy,
                         uint16_t trig, bool first, uint8_t *report)
{
    switch (type)
    {
    case APF_TYPE_KEYBOARD:
        report[0] = (uint8_t)(key >> 8);
        report[1] = 0;
        report[2] = (uint8_t)(joy >> 24);
        report[3] = (uint8_t)(joy >> 16);
        report[4] = (uint8_t)(joy >> 8);
        report[5] = (uint8_t)joy;
        report[6] = (uint8_t)(trig >> 8);
        report[7] = (uint8_t)trig;
        return 8;
    case APF_TYPE_MOUSE:
    {
        uint16_t dx = first ? 0 : apf_swap16(joy);
        uint16_t dy = first ? 0 : apf_swap16(trig);
        report[0] = (uint8_t)(joy >> 16);
        report[1] = 0;
        report[2] = (uint8_t)dx;
        report[3] = (uint8_t)(dx >> 8);
        report[4] = (uint8_t)dy;
        report[5] = (uint8_t)(dy >> 8);
        return 6;
    }
    }
    return 0;
}

/* The controller at a port, or no controller (an id of 0). */
static void apf_pad(int slot, uint8_t type, uint32_t key, uint32_t joy, uint16_t trig,
                    gamepad_host_t *pad)
{
    memset(pad, 0, sizeof *pad);
    if (type != APF_TYPE_POCKET && type != APF_TYPE_PAD && type != APF_TYPE_PAD_ANA)
        return;
    pad->id = (uint64_t)slot + 1;
    pad->type = type == APF_TYPE_POCKET ? GAMEPAD_TYPE_EASTERN : GAMEPAD_TYPE_UNKNOWN;
    uint16_t buttons = 0;
    for (int i = 0; i < 16; i++)
        if (apf_button_bits[i] != 0xFF && (key >> apf_button_bits[i] & 1))
            buttons |= (uint16_t)(1u << i);
    pad->button0 = (uint8_t)buttons;
    pad->button1 = (uint8_t)(buttons >> 8);
    pad->dpad = (uint8_t)(key & 0x0F);
    if (type != APF_TYPE_PAD_ANA)
        return;
    pad->sticks = true;
    pad->lx = hid_scale_analog_signed(joy & 0xFF, 8, 0, 255);
    pad->ly = hid_scale_analog_signed(joy >> 8 & 0xFF, 8, 0, 255);
    pad->rx = hid_scale_analog_signed(joy >> 16 & 0xFF, 8, 0, 255);
    pad->ry = hid_scale_analog_signed(joy >> 24, 8, 0, 255);
    pad->lt = hid_scale_analog(trig & 0xFF, 8, 0, 255);
    pad->rt = hid_scale_analog(trig >> 8, 8, 0, 255);
}

/* The controllers are told together, as libretro's are, and only as they
 * change. */
static void apf_pads_tell(void)
{
    apf_pads.n = MMIO_CONT_SLOTS;
    apf_pads.opened = apf_pads.fixed = true;
    if (!apf_pads_stale && !memcmp(&apf_pads_told, &apf_pads, sizeof apf_pads))
        return;
    memcpy(&apf_pads_told, &apf_pads, sizeof apf_pads);
    apf_pads_stale = false;
    event_gamepads(&apf_pads);
}

static void apf_slot_task(int slot)
{
    uint32_t key = MMIO_CONT_KEY(slot);
    uint8_t type = (uint8_t)(key >> 28);

    if (type != apf_slots[slot].type)
    {
        apf_umount(slot);
        memset(&apf_slots[slot], 0, sizeof(apf_slots[slot]));
        apf_slots[slot].slot = -1;
        apf_slots[slot].type = type;
        apf_mount(slot, type);
    }

    apf_pad(slot, type, key, MMIO_CONT_JOY(slot), (uint16_t)MMIO_CONT_TRIG(slot),
            &apf_pads.pad[slot]);
    if (apf_pads.pad[slot].id)
        return;

    bool first = false;
    if (type == APF_TYPE_MOUSE)
    {
        uint16_t count = apf_swap16(key);
        if (apf_slots[slot].mouse_have_seen && count == apf_slots[slot].mouse_seen)
            return;
        first = !apf_slots[slot].mouse_have_seen;
        apf_slots[slot].mouse_seen = count;
        apf_slots[slot].mouse_have_seen = true;
    }

    uint8_t report[APF_REPORT_MAX];
    uint8_t len = apf_build(type, key, MMIO_CONT_JOY(slot),
                            (uint16_t)MMIO_CONT_TRIG(slot), first, report);
    if (!len)
        return;
    if (type == APF_TYPE_MOUSE || apf_changed(slot, report, len))
        apf_report(slot, report, len);
}

void apf_refresh(void)
{
    for (int slot = 0; slot < MMIO_CONT_SLOTS; slot++)
    {
        apf_slots[slot].len = 0;
        apf_slots[slot].mouse_have_seen = false;
    }
    apf_pads_stale = true;
}

void apf_init(void)
{
    for (int slot = 0; slot < MMIO_CONT_SLOTS; slot++)
    {
        memset(&apf_slots[slot], 0, sizeof(apf_slots[slot]));
        apf_slots[slot].slot = -1;
    }
    memset(&apf_pads, 0, sizeof apf_pads);
    apf_pads_stale = true;
}

void apf_task(void)
{
    for (int slot = 0; slot < MMIO_CONT_SLOTS; slot++)
        apf_slot_task(slot);
    apf_pads_tell();
}
