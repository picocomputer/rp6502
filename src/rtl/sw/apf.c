/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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

#define APF_TYPE_NONE 0
#define APF_TYPE_POCKET 1
#define APF_TYPE_PAD 2
#define APF_TYPE_PAD_ANA 3
#define APF_TYPE_KBD 4
#define APF_TYPE_MOU 5

static const uint8_t apf_pad_desc[] = {
    0x05, 0x01,
    0x09, 0x05,
    0xa1, 0x01,

    0x05, 0x09,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x01,

    0x09, 0x11,
    0x81, 0x02,
    0x09, 0x12,
    0x81, 0x02,
    0x09, 0x13,
    0x81, 0x02,
    0x09, 0x14,
    0x81, 0x02,
    0x09, 0x01,
    0x81, 0x02,
    0x09, 0x02,
    0x81, 0x02,
    0x09, 0x04,
    0x81, 0x02,
    0x09, 0x05,
    0x81, 0x02,
    0x09, 0x07,
    0x81, 0x02,
    0x09, 0x08,
    0x81, 0x02,
    0x09, 0x09,
    0x81, 0x02,
    0x09, 0x0A,
    0x81, 0x02,
    0x09, 0x0E,
    0x81, 0x02,
    0x09, 0x0F,
    0x81, 0x02,
    0x09, 0x0B,
    0x81, 0x02,
    0x09, 0x0C,
    0x81, 0x02,

    0x05, 0x01,
    0x15, 0x00,
    0x26, 0xff, 0x00,
    0x75, 0x08,
    0x95, 0x01,
    0x09, 0x30,
    0x81, 0x02,
    0x09, 0x31,
    0x81, 0x02,
    0x09, 0x32,
    0x81, 0x02,
    0x09, 0x35,
    0x81, 0x02,
    0x09, 0x33,
    0x81, 0x02,
    0x09, 0x34,
    0x81, 0x02,

    0xc0,
};

static const uint8_t apf_kbd_desc[] = {
    0x05, 0x01,
    0x09, 0x06,
    0xa1, 0x01,

    0x05, 0x07,
    0x19, 0xe0,
    0x29, 0xe7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x01,
    0x19, 0x00,
    0x29, 0xff,
    0x15, 0x00,
    0x26, 0xff, 0x00,
    0x75, 0x08,
    0x95, 0x06,
    0x81, 0x00,

    0xc0,
};

static const uint8_t apf_mou_desc[] = {
    0x05, 0x01,
    0x09, 0x02,
    0xa1, 0x01,
    0x09, 0x01,
    0xa1, 0x00,

    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x08,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,

    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x01,

    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x16, 0x00, 0x80,
    0x26, 0xff, 0x7f,
    0x75, 0x10,
    0x95, 0x02,
    0x81, 0x06,

    0xc0,
    0xc0,
};

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
    case APF_TYPE_POCKET:
    case APF_TYPE_PAD:
        joy = 0x80808080u;
        trig = 0;
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
    if (type == APF_TYPE_MOU || apf_changed(slot, report, len))
        apf_report(slot, report, len);
}

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
