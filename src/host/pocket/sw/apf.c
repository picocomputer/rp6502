/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * APF's controller slots as HID devices — this platform's usb.c.
 *
 * Each slot is mounted with a descriptor written here and its registers
 * handed over as the report that descriptor describes, so ria/hid drives
 * them. ria/usb/xin.c does the same for XInput.
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
#include "core/hid/tab.h"

#include <stdint.h>
#include <string.h>

/* APF's device types, from the controller data documentation. */
#define APF_TYPE_NONE 0
#define APF_TYPE_POCKET 1  /* the Pocket's own buttons, player one */
#define APF_TYPE_PAD 2     /* docked controller, no analog */
#define APF_TYPE_PAD_ANA 3 /* docked controller, with analog */
#define APF_TYPE_KBD 4
#define APF_TYPE_MOU 5

/* The button usages put each bit where the XRAM report wants it: pad.c
 * files usage n at index n-1, packs 0-15 into its two button bytes, and
 * reads 16-19 as the d-pad. This one has no axes, for the two controller
 * types that have none. */
static const uint8_t apf_pad_desc[] = {
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x05, // Usage (Game Pad)
    0xa1, 0x01, // Collection (Application)

    0x05, 0x09, // Usage Page (Button)
    0x15, 0x00, // Logical Minimum (0)
    0x25, 0x01, // Logical Maximum (1)
    0x75, 0x01, // Report Size (1)
    0x95, 0x01, // Report Count (1)

    0x09, 0x11, // bit 0: dpad up      -> index 16
    0x81, 0x02, // Input (Data,Var,Abs)
    0x09, 0x12, // bit 1: dpad down    -> index 17
    0x81, 0x02,
    0x09, 0x13, // bit 2: dpad left    -> index 18
    0x81, 0x02,
    0x09, 0x14, // bit 3: dpad right   -> index 19
    0x81, 0x02,
    0x09, 0x01, // bit 4: A            -> index 0
    0x81, 0x02,
    0x09, 0x02, // bit 5: B            -> index 1
    0x81, 0x02,
    0x09, 0x04, // bit 6: X            -> index 3
    0x81, 0x02,
    0x09, 0x05, // bit 7: Y            -> index 4
    0x81, 0x02,
    0x09, 0x07, // bit 8: L1           -> index 6
    0x81, 0x02,
    0x09, 0x08, // bit 9: R1           -> index 7
    0x81, 0x02,
    0x09, 0x09, // bit 10: L2          -> index 8
    0x81, 0x02,
    0x09, 0x0A, // bit 11: R2          -> index 9
    0x81, 0x02,
    0x09, 0x0E, // bit 12: L3          -> index 13
    0x81, 0x02,
    0x09, 0x0F, // bit 13: R3          -> index 14
    0x81, 0x02,
    0x09, 0x0B, // bit 14: select      -> index 10
    0x81, 0x02,
    0x09, 0x0C, // bit 15: start       -> index 11
    0x81, 0x02,

    0xc0, // End Collection
};

/* The same buttons plus the axes, for the one controller type that has
 * them. Describing axes a digital pad does not have is how pad.c would come
 * to claim analog sticks it cannot read. */
static const uint8_t apf_pad_ana_desc[] = {
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x05, // Usage (Game Pad)
    0xa1, 0x01, // Collection (Application)

    0x05, 0x09, // Usage Page (Button)
    0x15, 0x00, // Logical Minimum (0)
    0x25, 0x01, // Logical Maximum (1)
    0x75, 0x01, // Report Size (1)
    0x95, 0x01, // Report Count (1)

    0x09, 0x11, // bit 0: dpad up      -> index 16
    0x81, 0x02, // Input (Data,Var,Abs)
    0x09, 0x12, // bit 1: dpad down    -> index 17
    0x81, 0x02,
    0x09, 0x13, // bit 2: dpad left    -> index 18
    0x81, 0x02,
    0x09, 0x14, // bit 3: dpad right   -> index 19
    0x81, 0x02,
    0x09, 0x01, // bit 4: A            -> index 0
    0x81, 0x02,
    0x09, 0x02, // bit 5: B            -> index 1
    0x81, 0x02,
    0x09, 0x04, // bit 6: X            -> index 3
    0x81, 0x02,
    0x09, 0x05, // bit 7: Y            -> index 4
    0x81, 0x02,
    0x09, 0x07, // bit 8: L1           -> index 6
    0x81, 0x02,
    0x09, 0x08, // bit 9: R1           -> index 7
    0x81, 0x02,
    0x09, 0x09, // bit 10: L2          -> index 8
    0x81, 0x02,
    0x09, 0x0A, // bit 11: R2          -> index 9
    0x81, 0x02,
    0x09, 0x0E, // bit 12: L3          -> index 13
    0x81, 0x02,
    0x09, 0x0F, // bit 13: R3          -> index 14
    0x81, 0x02,
    0x09, 0x0B, // bit 14: select      -> index 10
    0x81, 0x02,
    0x09, 0x0C, // bit 15: start       -> index 11
    0x81, 0x02,

    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x15, 0x00,       // Logical Minimum (0)
    0x26, 0xff, 0x00, // Logical Maximum (255)
    0x75, 0x08,       // Report Size (8)
    0x95, 0x01,       // Report Count (1)
    0x09, 0x30,       // X  (left stick X)
    0x81, 0x02,
    0x09, 0x31, // Y  (left stick Y)
    0x81, 0x02,
    0x09, 0x32, // Z  (right stick X)
    0x81, 0x02,
    0x09, 0x35, // Rz (right stick Y)
    0x81, 0x02,
    0x09, 0x33, // Rx (left trigger)
    0x81, 0x02,
    0x09, 0x34, // Ry (right trigger)
    0x81, 0x02,

    0xc0, // End Collection
};

/* A boot keyboard: modifier byte, unused byte, six scan codes. */
static const uint8_t apf_kbd_desc[] = {
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x06, // Usage (Keyboard)
    0xa1, 0x01, // Collection (Application)

    0x05, 0x07,       // Usage Page (Keyboard)
    0x19, 0xe0,       // Usage Minimum (224)
    0x29, 0xe7,       // Usage Maximum (231)
    0x15, 0x00,       // Logical Minimum (0)
    0x25, 0x01,       // Logical Maximum (1)
    0x75, 0x01,       // Report Size (1)
    0x95, 0x08,       // Report Count (8)
    0x81, 0x02,       // Input (Data,Var,Abs) - the modifiers
    0x75, 0x08,       // Report Size (8)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x01,       // Input (Const) - the reserved byte
    0x19, 0x00,       // Usage Minimum (0)
    0x29, 0xff,       // Usage Maximum (255)
    0x15, 0x00,       // Logical Minimum (0)
    0x26, 0xff, 0x00, // Logical Maximum (255)
    0x75, 0x08,       // Report Size (8)
    0x95, 0x06,       // Report Count (6)
    0x81, 0x00,       // Input (Data,Array) - the six scan codes

    0xc0, // End Collection
};

/* Eight buttons and two 16-bit movements. Buttons, X and Y are all the
 * docked mouse documents, so there is no wheel to describe. */
static const uint8_t apf_mou_desc[] = {
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x02, // Usage (Mouse)
    0xa1, 0x01, // Collection (Application)
    0x09, 0x01, // Usage (Pointer)
    0xa1, 0x00, // Collection (Physical)

    0x05, 0x09, // Usage Page (Button)
    0x19, 0x01, // Usage Minimum (1)
    0x29, 0x08, // Usage Maximum (8)
    0x15, 0x00, // Logical Minimum (0)
    0x25, 0x01, // Logical Maximum (1)
    0x75, 0x01, // Report Size (1)
    0x95, 0x08, // Report Count (8)
    0x81, 0x02, // Input (Data,Var,Abs)

    0x75, 0x08, // Report Size (8)
    0x95, 0x01, // Report Count (1)
    0x81, 0x01, // Input (Const) - pad to the movements

    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x30,       // Usage (X)
    0x09, 0x31,       // Usage (Y)
    0x16, 0x00, 0x80, // Logical Minimum (-32768)
    0x26, 0xff, 0x7f, // Logical Maximum (32767)
    0x75, 0x10,       // Report Size (16)
    0x95, 0x02,       // Report Count (2)
    0x81, 0x06,       // Input (Data,Var,Rel)

    0xc0, // End Collection
    0xc0, // End Collection
};

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
    uint8_t report[APF_REPORT_MAX];
    uint8_t len;
    uint16_t mou_seen;
    bool mou_have_seen;
} apf_slots[MMIO_CONT_SLOTS];

static int apf_hid_slot(int slot)
{
    return HID_APF_START + slot;
}

/* Offered to every driver, the way a USB report descriptor is: each one
 * keeps what it recognizes. */
static void apf_mount(int slot, const uint8_t *desc, uint16_t len, uint8_t button_type)
{
    int hid_slot = apf_hid_slot(slot);
    kbd_mount(hid_slot, desc, len, 0, 0);
    mou_mount(hid_slot, desc, len);
    tab_mount(hid_slot, desc, len);
    pad_mount(hid_slot, desc, len, 0, 0, button_type);
}

static void apf_umount(int slot)
{
    int hid_slot = apf_hid_slot(slot);
    kbd_umount(hid_slot);
    mou_umount(hid_slot);
    tab_umount(hid_slot);
    pad_umount(hid_slot);
}

static void apf_report(int slot, const uint8_t *report, uint8_t len)
{
    int hid_slot = apf_hid_slot(slot);
    kbd_report(hid_slot, report, len);
    mou_report(hid_slot, report, len);
    tab_report(hid_slot, report, len);
    pad_report(hid_slot, report, len);
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
        apf_slots[slot].type = type;
        switch (type)
        {
        /* The Pocket's own buttons are the only ones here whose labels we
         * know: an A east of a B, the way Nintendo arranges them. Whatever
         * is in the dock arrives through the same normalized bitmap, so it
         * could be wearing any labels at all. */
        case APF_TYPE_POCKET:
            apf_mount(slot, apf_pad_desc, sizeof(apf_pad_desc), PAD_TYPE_EASTERN);
            break;
        case APF_TYPE_PAD:
            apf_mount(slot, apf_pad_desc, sizeof(apf_pad_desc), PAD_TYPE_UNKNOWN);
            break;
        case APF_TYPE_PAD_ANA:
            apf_mount(slot, apf_pad_ana_desc, sizeof(apf_pad_ana_desc), PAD_TYPE_UNKNOWN);
            break;
        case APF_TYPE_KBD:
            apf_mount(slot, apf_kbd_desc, sizeof(apf_kbd_desc), PAD_TYPE_UNKNOWN);
            break;
        case APF_TYPE_MOU:
            apf_mount(slot, apf_mou_desc, sizeof(apf_mou_desc), PAD_TYPE_UNKNOWN);
            break;
        }
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
        apf_umount(slot);
        memset(&apf_slots[slot], 0, sizeof(apf_slots[slot]));
    }
}

void apf_task(void)
{
    for (int slot = 0; slot < MMIO_CONT_SLOTS; slot++)
        apf_slot_task(slot);
}
