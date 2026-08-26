/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * APF's controller slots as HID devices — this platform's usb.c.
 *
 * A slot is mounted by handing ria/hid the driver structs written here,
 * and its registers are handed over as the report those structs
 * describe. No descriptor is involved, so this machine does not carry a
 * parser. ria/usb/xin.c does the same for XInput.
 *
 * A slot's type is the top nibble of its key word, not the slot number:
 * any slot may hold any device, and a changed nibble is a device
 * unplugged and another plugged in.
 */

#include "apf.h"
#include "mmio.h"

#include "core/hid/hid.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include "core/hid/usage.h"

#include <stdint.h>
#include <string.h>

/* APF's device types, from the controller data documentation. */
#define APF_TYPE_NONE 0
#define APF_TYPE_POCKET 1  /* the Pocket's own buttons, player one */
#define APF_TYPE_PAD 2     /* docked controller, no analog */
#define APF_TYPE_PAD_ANA 3 /* docked controller, with analog */
#define APF_TYPE_KEYBOARD 4
#define APF_TYPE_MOUSE 5

/* What the drivers are told a device is. A USB device says this with a
 * report descriptor; the dock has none to say it with, so where every
 * field sits is stated outright -- these are the bit positions apf_build
 * writes to.
 *
 * The button numbers are the ones gamepad.c files at index n-1: 1-16 land in
 * the two button bytes and 17-20 are read as the d-pad. */
static const gamepad_connection_t apf_gamepad_desc = {
    .valid = true,
    .x_absolute = true,
    .button_offsets = {
        4, 5, HID_ABSENT, 6, 7, HID_ABSENT, 8, 9,
        10, 11, 14, 15, HID_ABSENT, 12, 13, HID_ABSENT,
        0, 1, 2, 3}};

// The same gamepad, with two sticks and two triggers a byte each behind it.
static const gamepad_connection_t apf_gamepad_ana_desc = {
    .valid = true,
    .x_absolute = true,
    .x_offset = 2 * 8, .x_size = 8, .x_max = 255,
    .y_offset = 3 * 8, .y_size = 8, .y_max = 255,
    .z_offset = 4 * 8, .z_size = 8, .z_max = 255,
    .rz_offset = 5 * 8, .rz_size = 8, .rz_max = 255,
    .rx_offset = 6 * 8, .rx_size = 8, .rx_max = 255,
    .ry_offset = 7 * 8, .ry_size = 8, .ry_max = 255,
    .button_offsets = {
        4, 5, HID_ABSENT, 6, 7, HID_ABSENT, 8, 9,
        10, 11, 14, 15, HID_ABSENT, 12, 13, HID_ABSENT,
        0, 1, 2, 3}};

/* A boot keyboard's report: the modifier byte, a reserved byte, then six
 * scan codes. */
static const keyboard_connection_t apf_keyboard_desc = {
    .valid = true,
    .runs = {{.bit_pos = 0, .usage_min = HID_KEY_CONTROL_LEFT, .count = 8}},
    .codes_offset = 2 * 8,
    .codes_count = 6};

/* Eight buttons, a byte of padding, then two 16-bit movements. Buttons,
 * X and Y are all the docked mouse documents, so there is no wheel. */
static const mouse_connection_t apf_mouse_desc = {
    .valid = true,
    .button_offsets = {0, 1, 2, 3, 4, 5, 6, 7},
    .x_relative = true,
    .x_offset = 2 * 8, .x_size = 16,
    .y_offset = 4 * 8, .y_size = 16};

/* The same mouse read as a pointer, which is what a USB mouse gets too:
 * its first button is the tip a program draws with. */
static const tablet_connection_t apf_mouse_tablet_desc = {
    .valid = true,
    .button_offsets = {0, 1, 2, 3, 4},
    .x_relative = true,
    .x_offset = 2 * 8, .x_size = 16, .x_min = -32768, .x_max = 32767,
    .y_offset = 4 * 8, .y_size = 16, .y_min = -32768, .y_max = 32767,
    .wheel_offset = HID_ABSENT,
    .pan_offset = HID_ABSENT,
    .tip_offset = HID_ABSENT,
    .inrange_offset = HID_ABSENT};

/* APF packs word bytes most significant first, then documents the
 * keyboard's modifiers and the mouse's movements as little endian. Taken
 * together, such a field's low byte sits in the high half of its
 * halfword. Single-byte fields need none of this. */
static inline uint16_t apf_swap16(uint32_t word)
{
    return (uint16_t)(((word & 0xFFu) << 8) | ((word >> 8) & 0xFFu));
}

#define APF_REPORT_MAX 8

static struct
{
    uint8_t type;
    int8_t slot; // where ria/hid mounted it, -1 for nothing
    uint8_t report[APF_REPORT_MAX];
    uint8_t len;
    uint16_t mouse_seen;
    bool mouse_have_seen;
} apf_slots[MMIO_CONT_SLOTS];

static void apf_mount(int slot, uint8_t type)
{
    const gamepad_connection_t *gamepad = NULL;
    uint8_t button_type = GAMEPAD_TYPE_UNKNOWN;
    switch (type)
    {
    /* The Pocket's own buttons are the only ones here whose labels we
     * know: an A east of a B, the way Nintendo arranges them. Whatever
     * is in the dock arrives through the same normalized bitmap, so it
     * could be wearing any labels at all. */
    case APF_TYPE_POCKET:
        button_type = GAMEPAD_TYPE_EASTERN;
        gamepad = &apf_gamepad_desc;
        break;
    case APF_TYPE_PAD:
        gamepad = &apf_gamepad_desc;
        break;
    case APF_TYPE_PAD_ANA:
        gamepad = &apf_gamepad_ana_desc;
        break;
    case APF_TYPE_KEYBOARD:
        apf_slots[slot].slot = (int8_t)hid_mount(&apf_keyboard_desc, NULL, NULL, NULL, 0, 0, 0);
        return;
    case APF_TYPE_MOUSE:
        apf_slots[slot].slot = (int8_t)hid_mount(NULL, &apf_mouse_desc,
                                                 &apf_mouse_tablet_desc, NULL, 0, 0, 0);
        return;
    default:
        return;
    }
    apf_slots[slot].slot = (int8_t)hid_mount(NULL, NULL, NULL, gamepad, 0, 0, button_type);
}

static void apf_umount(int slot)
{
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

/* Reads no hardware, so tests/hid can ask what a given set of registers
 * types, points at or presses. Zero for a slot holding nothing. */
static uint8_t apf_build(uint8_t type, uint32_t key, uint32_t joy,
                         uint16_t trig, bool first, uint8_t *report)
{
    switch (type)
    {
    case APF_TYPE_POCKET:
    case APF_TYPE_PAD:
        /* Their descriptor stops after the buttons, so the axis words are
         * not ours to send. gamepad.c centres what it isn't told and reads a
         * pressed L2/R2 as a full trigger. */
        report[0] = (uint8_t)key;
        report[1] = (uint8_t)(key >> 8);
        return 2;
    case APF_TYPE_PAD_ANA:
        report[0] = (uint8_t)key;
        report[1] = (uint8_t)(key >> 8);
        report[2] = (uint8_t)joy;
        report[3] = (uint8_t)(joy >> 8);
        report[4] = (uint8_t)(joy >> 16);
        report[5] = (uint8_t)(joy >> 24);
        report[6] = (uint8_t)trig;
        report[7] = (uint8_t)(trig >> 8);
        return 8;
    case APF_TYPE_KEYBOARD:
        /* The modifier field's first byte, so the high half of its
         * halfword. */
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
        /* The first report's deltas are however far the hand went before
         * anyone was listening. */
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

    /* A mouse sends reports; everything else holds a level. Its counter
     * is the only way to tell a hand that did not move from a report that
     * never came, and reading the same delta twice slides the pointer. */
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
    /* An unmoved level is the same report, and resending it would repeat
     * a key at the speed of the loop. A mouse passed its counter above. */
    if (type == APF_TYPE_MOUSE || apf_changed(slot, report, len))
        apf_report(slot, report, len);
}

/* Mapping blanks the record, on the reasoning that a device will report
 * again shortly. That holds over USB but not for levels: a stick held
 * off centre produces the same report forever, the dedup never sends it,
 * and the program reads the blank until the hand moves. The mouse is
 * paced by its counter instead, so it is marked unseen. */
void apf_refresh(void)
{
    for (int slot = 0; slot < MMIO_CONT_SLOTS; slot++)
    {
        apf_slots[slot].len = 0;
        apf_slots[slot].mouse_have_seen = false;
    }
}

void apf_init(void)
{
    for (int slot = 0; slot < MMIO_CONT_SLOTS; slot++)
    {
        memset(&apf_slots[slot], 0, sizeof(apf_slots[slot]));
        apf_slots[slot].slot = -1;
    }
}

void apf_task(void)
{
    for (int slot = 0; slot < MMIO_CONT_SLOTS; slot++)
        apf_slot_task(slot);
}
