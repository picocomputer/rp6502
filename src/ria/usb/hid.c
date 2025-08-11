/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb.h"
#include "usb/hid.h"
#include "hid/kbd.h"
#include "hid/mou.h"
#include "hid/pad.h"
#include "usb/xin.h"

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_HID)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static bool hid_leds_dirty;
static uint8_t hid_leds;
static uint8_t hid_count_kbd;
static uint8_t hid_count_mou;
static uint8_t hid_count_pad;

void hid_set_leds(uint8_t leds)
{
    hid_leds = leds;
    hid_leds_dirty = true;
}

void hid_task(void)
{
    if (hid_leds_dirty)
    {
        hid_leds_dirty = false;
        for (uint8_t dev_addr = 1; dev_addr <= CFG_TUH_DEVICE_MAX; dev_addr++)
            for (uint8_t idx = 0; idx < CFG_TUH_HID; idx++)
                if (tuh_hid_interface_protocol(dev_addr, idx) == HID_ITF_PROTOCOL_KEYBOARD)
                    tuh_hid_set_report(dev_addr, idx, 0, HID_REPORT_TYPE_OUTPUT,
                                       &hid_leds, sizeof(hid_leds));
    }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *report, uint16_t len)
{
    kbd_report(idx, report, len);
    mou_report(idx, report, len);
    pad_report(idx, report, len);
    tuh_hid_receive_report(dev_addr, idx);
}

void hid_print_status(void)
{
    printf("%d keyboard%s, %d %s",
           hid_count_kbd, hid_count_kbd == 1 ? "" : "s",
           hid_count_mou, hid_count_mou == 1 ? "mouse" : "mice");
}

int hid_pad_count(void)
{
    return hid_count_pad;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *desc_report, uint16_t desc_len)
{
    bool valid = false;

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);

    uint16_t vendor_id;
    uint16_t product_id;
    tuh_vid_pid_get(dev_addr, &vendor_id, &product_id);

    DBG("HID device mounted: dev_addr=%d, idx=%d, protocol=%d, desc_len=%d\n",
        dev_addr, idx, itf_protocol, desc_len);

    if (kbd_mount(idx, desc_report, desc_len))
    {
        ++hid_count_kbd;
        hid_leds_dirty = true; // TODO this stopped working
        valid = true;
    }
    if (mou_mount(idx, desc_report, desc_len))
    {
        ++hid_count_mou;
        valid = true;
    }
    if (pad_mount(idx, desc_report, desc_len, vendor_id, product_id))
    {
        ++hid_count_pad;
        valid = true;
    }

    if (valid)
        tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    (void)dev_addr;
    if (kbd_umount(idx))
        --hid_count_kbd;
    if (mou_umount(idx))
        --hid_count_mou;
    if (pad_umount(idx))
        --hid_count_pad;
}
