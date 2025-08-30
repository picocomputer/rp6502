/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hid/hid.h"
#include "hid/kbd.h"
#include "hid/mou.h"
#include "hid/pad.h"
#include "usb/msc.h"
#include "usb/usb.h"
#include "usb/xin.h"
#include <tusb.h>

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_USB)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static bool usb_hid_leds_dirty;
static uint8_t usb_hid_leds;
static uint8_t usb_count_hid_kbd;
static uint8_t usb_count_hid_mou;
static uint8_t usb_count_hid_pad;

void usb_init(void)
{
    tuh_init(TUH_OPT_RHPORT);
    tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
}

void usb_task(void)
{
    tuh_task();
    if (usb_hid_leds_dirty)
    {
        usb_hid_leds_dirty = false;
        for (uint8_t dev_addr = 1; dev_addr <= CFG_TUH_DEVICE_MAX; dev_addr++)
            for (uint8_t idx = 0; idx < CFG_TUH_HID; idx++)
                if (tuh_hid_interface_protocol(dev_addr, idx) == HID_ITF_PROTOCOL_KEYBOARD)
                    tuh_hid_set_report(dev_addr, idx, 0, HID_REPORT_TYPE_OUTPUT,
                                       &usb_hid_leds, sizeof(usb_hid_leds));
    }
}

void usb_print_status(void)
{
    int count_gamepad = usb_count_hid_pad + xin_pad_count();
    printf("USB : ");
    printf("%d keyboard%s, %d %s",
           usb_count_hid_kbd, usb_count_hid_kbd == 1 ? "" : "s",
           usb_count_hid_mou, usb_count_hid_mou == 1 ? "mouse" : "mice");
    printf(", %d gamepad%s", count_gamepad, count_gamepad == 1 ? "" : "s");
    msc_print_status();
}

void usb_set_hid_leds(uint8_t leds)
{
    usb_hid_leds = leds;
    usb_hid_leds_dirty = true;
}

static inline int usb_idx_to_hid_slot(int idx)
{
    return HID_USB_START + idx;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *report, uint16_t len)
{
    kbd_report(usb_idx_to_hid_slot(idx), report, len);
    mou_report(usb_idx_to_hid_slot(idx), report, len);
    pad_report(usb_idx_to_hid_slot(idx), report, len);
    tuh_hid_receive_report(dev_addr, idx);
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

    if (kbd_mount(usb_idx_to_hid_slot(idx), desc_report, desc_len))
    {
        ++usb_count_hid_kbd;
        usb_hid_leds_dirty = true; // TODO this stopped working
        valid = true;
    }
    if (mou_mount(usb_idx_to_hid_slot(idx), desc_report, desc_len))
    {
        ++usb_count_hid_mou;
        valid = true;
    }
    if (pad_mount(usb_idx_to_hid_slot(idx), desc_report, desc_len, vendor_id, product_id))
    {
        ++usb_count_hid_pad;
        valid = true;
    }

    if (valid)
        tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    (void)dev_addr;
    if (kbd_umount(usb_idx_to_hid_slot(idx)))
        --usb_count_hid_kbd;
    if (mou_umount(usb_idx_to_hid_slot(idx)))
        --usb_count_hid_mou;
    if (pad_umount(usb_idx_to_hid_slot(idx)))
        --usb_count_hid_pad;
}
