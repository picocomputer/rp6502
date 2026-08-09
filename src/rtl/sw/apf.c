/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * APF's controller slots as HID devices — this platform's usb.c.
 *
 * The dock's keyboard, its mouse and up to four gamepads all arrive as
 * three registers per slot, and the machine already has drivers for all
 * of them: ria/hid, which speaks report descriptors. So rather than a
 * second set of drivers that republish the same XRAM by hand, each slot
 * is mounted with a descriptor written here and its registers are
 * handed over as the report that descriptor describes. ria/usb/xin.c
 * does exactly this for XInput, which has no descriptor either.
 *
 * What that buys is everything the hand-written ones did not have: the
 * keyboard's layouts, dead keys, repeat and console, the mouse landing
 * in the tablet driver as well, the pad's straight-through mapping, and
 * four players instead of one.
 *
 * A slot says what it holds in the top nibble of its key word, and
 * Analogue's own note is that the assignment is the framework's to
 * make. So the nibble is what is trusted, not the slot number: any
 * slot may hold any device, and a slot whose nibble changes is a device
 * unplugged and another plugged in.
 */

#include "apf.h"
#include "mmio.h"

#include "ria/hid/hid.h"
#include "ria/hid/kbd.h"
#include "ria/hid/mou.h"
#include "ria/hid/pad.h"
#include "ria/hid/tab.h"

#include <stdint.h>
#include <string.h>

/* APF's device types, from the controller data documentation. */
#define APF_TYPE_NONE 0
#define APF_TYPE_POCKET 1  /* the Pocket's own buttons, player one */
#define APF_TYPE_PAD 2     /* docked controller, no analog */
#define APF_TYPE_PAD_ANA 3 /* docked controller, with analog */
#define APF_TYPE_KBD 4
#define APF_TYPE_MOU 5

/* The gamepad. Sixteen buttons in APF's own bit order, then the four
 * stick axes and the two triggers, which is exactly the eight bytes
 * assembled below. The button usages are what puts each one where the
 * XRAM report wants it: pad.c files button usage n at index n-1, packs
 * indices 0-15 into its two button bytes, and reads indices 16-19 as
 * the d-pad. Straight through, every one of them — the dpad is a dpad
 * and the sticks are sticks. */
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

/* The keyboard, which is a boot keyboard: the modifier byte, a byte
 * nobody uses, and six scan codes. That is the shape APF sends and the
 * shape kbd.c has always parsed. */
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

/* The mouse: eight buttons and two sixteen-bit movements. Analogue
 * documents buttons, X and Y for the docked mouse and nothing else, so
 * there is no wheel here to describe. mou.c takes it as a mouse and
 * tab.c takes the same report as a pointer, the way a USB mouse lands
 * in both. */
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

/* A sixteen-bit field, the way one actually arrives.
 *
 * APF packs the bytes of a word most significant first — its own table
 * puts the keyboard's first scan code in joy[31:24] and its fourth in
 * joy[7:0] — and then documents three fields as "little endian byte
 * order": the keyboard's modifiers and the mouse's two movements. Byte
 * order is only worth stating for a value made of more than one byte,
 * and taken together the two statements put such a field's low byte in
 * the high half of its half word.
 *
 * Read as it stands, a movement of three is 0x0300 and a keyboard's
 * shift is eight bits from where it is looked for. The six scan codes
 * are single bytes and need none of this, which is why typing worked
 * while nothing else did. */
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
 * looks at what it was given and keeps what it recognizes. A mouse is
 * a mouse to mou.c and a pointer to tab.c, and this is where that
 * happens rather than something decided here. */
static void apf_mount(int slot, const uint8_t *desc, uint16_t len)
{
    int hid_slot = apf_hid_slot(slot);
    kbd_mount(hid_slot, desc, len, 0, 0);
    mou_mount(hid_slot, desc, len);
    tab_mount(hid_slot, desc, len);
    pad_mount(hid_slot, desc, len, 0, 0);
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

/* True when this is news. The registers are levels, so a report that
 * has not changed is the same report and sending it again would be a
 * key repeating itself at the speed of the loop. */
static bool apf_changed(int slot, const uint8_t *report, uint8_t len)
{
    if (apf_slots[slot].len == len &&
        !memcmp(apf_slots[slot].report, report, len))
        return false;
    memcpy(apf_slots[slot].report, report, len);
    apf_slots[slot].len = len;
    return true;
}

/* The registers of one slot as the report its descriptor describes.
 * Nothing here reads hardware, which is what lets tests/hid ask what a
 * given set of registers types, points at or presses. Returns the
 * report's length, or zero for a slot holding nothing. */
static uint8_t apf_build(uint8_t type, uint32_t key, uint32_t joy,
                         uint16_t trig, bool first, uint8_t *report)
{
    switch (type)
    {
    case APF_TYPE_POCKET:
    case APF_TYPE_PAD:
        /* A pad without analog carries no axes, so the sticks are
         * centered rather than left at whatever the words held. The
         * triggers stay at zero; pad.c reads a pressed L2 or R2 as a
         * trigger fully down, so a digital pad still has both. */
        joy = 0x80808080u;
        trig = 0;
        /* fall through */
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
        /* The HID modifier byte, which is the modifier field's first
         * byte and so the high half of its half word. */
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
        /* The first report after a mouse appears carries no movement:
         * its deltas are however far the hand went before anyone was
         * listening. */
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
        case APF_TYPE_POCKET:
        case APF_TYPE_PAD:
        case APF_TYPE_PAD_ANA:
            apf_mount(slot, apf_pad_desc, sizeof(apf_pad_desc));
            break;
        case APF_TYPE_KBD:
            apf_mount(slot, apf_kbd_desc, sizeof(apf_kbd_desc));
            break;
        case APF_TYPE_MOU:
            apf_mount(slot, apf_mou_desc, sizeof(apf_mou_desc));
            break;
        }
    }

    /* A mouse sends reports and everything else holds a level. The
     * counter it stamps each report with is the only way to tell a hand
     * that did not move from a report that never came, and reading the
     * same delta twice would slide the pointer on its own. */
    bool first = false;
    if (type == APF_TYPE_MOU)
    {
        /* Swapped like the movements beside it, though only inequality
         * is ever asked of it. */
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
    /* A level that has not moved is the same report, and sending it
     * again would be a key repeating itself at the speed of the loop.
     * A mouse got past the counter above, so it is always news. */
    if (type == APF_TYPE_MOU || apf_changed(slot, report, len))
        apf_report(slot, report, len);
}

/* A program just mapped one of the drivers, and mapping blanks the
 * record it maps: pad_xreg and tab_xreg both write an empty one, on the
 * reasoning that a device will report again in a moment. That holds
 * over USB and does not hold here, where the registers are levels — a
 * stick held off centre produces the same report forever, the dedup
 * below never sends it, and the program reads the blank until the hand
 * moves. So forget what was last sent and let the next pass be news.
 *
 * The mouse is paced by its counter rather than the cache, so it is
 * marked unseen instead: the report that follows carries the buttons
 * and no movement, which is what a hand that has not moved is. */
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
