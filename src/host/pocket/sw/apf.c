/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * APF's controller slots as HID devices — this platform's usb.c.
 *
 * Each slot is mounted with a report map written here and its registers
 * handed over as the report that map describes, so ria/hid drives them.
 * ria/usb/xin.c does the same for XInput, from a descriptor.
 *
 * A slot's type is the top nibble of its key word, not the slot number:
 * any slot may hold any device, and a changed nibble is a device
 * unplugged and another plugged in.
 */

#include "apf.h"
#include "mmio.h"

#include "core/hid/hid.h"
#include "core/hid/kbd.h"
#include "core/hid/mou.h"
#include "core/hid/pad.h"
#include "core/hid/usage.h"

#include <stdint.h>
#include <string.h>

/* APF's device types, from the controller data documentation. */
#define APF_TYPE_NONE 0
#define APF_TYPE_POCKET 1  /* the Pocket's own buttons, player one */
#define APF_TYPE_PAD 2     /* docked controller, no analog */
#define APF_TYPE_PAD_ANA 3 /* docked controller, with analog */
#define APF_TYPE_KBD 4
#define APF_TYPE_MOU 5

/* What the drivers are told a device is. A USB device says this with a
 * report descriptor; the dock has none to say it with, so these are the
 * bit positions apf_build writes to, stated outright.
 *
 * pad.c files usage n at index n-1, packs 0-15 into its two button bytes
 * and reads 16-19 as the d-pad, so this is the order that puts each dock
 * button where the XRAM report wants it. */
static const uint16_t apf_pad_buttons[HID_MAP_BUTTONS] = {
    4, 5, HID_ABSENT, 6, 7, HID_ABSENT, 8, 9,
    10, 11, 14, 15, HID_ABSENT, 12, 13, HID_ABSENT,
    0, 1, 2, 3};

static void apf_map_pad(hid_report_map_t *map, bool analog)
{
    hid_map_clear(map);
    map->app_usage = HID_APP_GAMEPAD;
    memcpy(map->button_bit, apf_pad_buttons, sizeof(map->button_bit));
    if (!analog)
        return;
    // Two sticks then two triggers, a byte each, after the button bytes.
    static const uint8_t axes[] = {HID_AXIS_X, HID_AXIS_Y, HID_AXIS_Z,
                                   HID_AXIS_RZ, HID_AXIS_RX, HID_AXIS_RY};
    for (uint16_t i = 0; i < sizeof(axes); i++)
    {
        hid_locus_t *locus = &map->axis[axes[i]];
        locus->bit_pos = (uint16_t)(16 + i * 8);
        locus->size = 8;
        locus->logical_max = 255;
    }
}

/* A boot keyboard's report: the modifier byte, a reserved byte, then six
 * scan codes. */
static void apf_map_kbd(hid_report_map_t *map)
{
    hid_map_clear(map);
    map->app_usage = HID_APP_KEYBOARD;
    map->key_run[0].bit_pos = 0;
    map->key_run[0].usage_min = HID_KEY_CONTROL_LEFT;
    map->key_run[0].count = 8;
    map->key_array_bit = 16;
    map->key_array_count = 6;
}

/* Eight buttons, a byte of padding, then two 16-bit movements. Buttons,
 * X and Y are all the docked mouse documents, so there is no wheel. */
static void apf_map_mou(hid_report_map_t *map)
{
    hid_map_clear(map);
    map->app_usage = HID_APP_MOUSE;
    for (uint16_t i = 0; i < 8; i++)
        map->button_bit[i] = i;
    map->axis[HID_AXIS_X].bit_pos = 16;
    map->axis[HID_AXIS_X].size = 16;
    map->axis[HID_AXIS_X].relative = true;
    map->axis[HID_AXIS_X].logical_min = -32768;
    map->axis[HID_AXIS_X].logical_max = 32767;
    map->axis[HID_AXIS_Y] = map->axis[HID_AXIS_X];
    map->axis[HID_AXIS_Y].bit_pos = 32;
}

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
    uint16_t mou_seen;
    bool mou_have_seen;
} apf_slots[MMIO_CONT_SLOTS];

static void apf_mount(int slot, uint8_t type)
{
    hid_report_map_t map;
    uint8_t button_type = PAD_TYPE_UNKNOWN;
    switch (type)
    {
    /* The Pocket's own buttons are the only ones here whose labels we
     * know: an A east of a B, the way Nintendo arranges them. Whatever
     * is in the dock arrives through the same normalized bitmap, so it
     * could be wearing any labels at all. */
    case APF_TYPE_POCKET:
        button_type = PAD_TYPE_EASTERN;
        apf_map_pad(&map, false);
        break;
    case APF_TYPE_PAD:
        apf_map_pad(&map, false);
        break;
    case APF_TYPE_PAD_ANA:
        apf_map_pad(&map, true);
        break;
    case APF_TYPE_KBD:
        apf_map_kbd(&map);
        break;
    case APF_TYPE_MOU:
        apf_map_mou(&map);
        break;
    default:
        return;
    }
    apf_slots[slot].slot = (int8_t)hid_mount(&map, 0, 0, button_type);
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
         * not ours to send. pad.c centres what it isn't told and reads a
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
    case APF_TYPE_KBD:
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
    case APF_TYPE_MOU:
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
    if (type == APF_TYPE_MOU)
    {
        uint16_t count = apf_swap16(key);
        if (apf_slots[slot].mou_have_seen && count == apf_slots[slot].mou_seen)
            return;
        first = !apf_slots[slot].mou_have_seen;
        apf_slots[slot].mou_seen = count;
        apf_slots[slot].mou_have_seen = true;
    }

    uint8_t report[APF_REPORT_MAX];
    uint8_t len = apf_build(type, key, MMIO_CONT_JOY(slot),
                            (uint16_t)MMIO_CONT_TRIG(slot), first, report);
    if (!len)
        return;
    /* An unmoved level is the same report, and resending it would repeat
     * a key at the speed of the loop. A mouse passed its counter above. */
    if (type == APF_TYPE_MOU || apf_changed(slot, report, len))
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
        apf_slots[slot].mou_have_seen = false;
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
