/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb.h"
#include "usb/hid.h"
#include "usb/kbd.h"
#include "usb/mou.h"
#include "usb/pad.h"
#include "pico/time.h"

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_HID)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static uint8_t hid_dev_addr[CFG_TUH_HID];

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *report, uint16_t len)
{
    (void)len;

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);
    switch (itf_protocol)
    {
    case HID_ITF_PROTOCOL_KEYBOARD:
        kbd_report(idx, (hid_keyboard_report_t const *)report);
        break;
    case HID_ITF_PROTOCOL_MOUSE:
        mou_report((hid_mouse_report_t const *)report);
        break;
    case HID_ITF_PROTOCOL_NONE:
        pad_report(idx, report, len);
        break;
    }

    if (!tuh_hid_receive_report(dev_addr, idx))
        DBG("tuh_hid_receive_report failed");
}

void hid_print_status(void)
{
    int count_keyboard = 0;
    int count_mouse = 0;
    int count_gamepad = 0;
    for (int idx = 0; idx < CFG_TUH_HID; idx++)
    {
        uint8_t dev_addr = hid_dev_addr[idx];
        if (dev_addr)
        {
            switch (tuh_hid_interface_protocol(dev_addr, idx))
            {
            case HID_ITF_PROTOCOL_KEYBOARD:
                count_keyboard++;
                break;
            case HID_ITF_PROTOCOL_MOUSE:
                count_mouse++;
                break;
            case HID_ITF_PROTOCOL_NONE:
                if (pad_is_valid(idx))
                    count_gamepad++;
                break;
            }
        }
    }
    printf("USB HID: %d keyboard%s, %d %s, %d gamepad%s",
           count_keyboard, count_keyboard == 1 ? "" : "s",
           count_mouse, count_mouse == 1 ? "mouse" : "mice",
           count_gamepad, count_gamepad == 1 ? "\n" : "s\n");
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *desc_report, uint16_t desc_len)
{
    bool valid = false;
    hid_dev_addr[idx] = dev_addr;

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);

    DBG("HID device mounted: dev_addr=%d, idx=%d, protocol=%d, desc_len=%d\n",
        dev_addr, idx, itf_protocol, desc_len);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD)
    {
        valid = true;
        kbd_hid_leds_dirty();
    }
    else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE)
    {
        valid = true;
    }
    else if (itf_protocol == HID_ITF_PROTOCOL_NONE)
    {
        uint16_t vendor_id;
        uint16_t product_id;
        if (tuh_vid_pid_get(dev_addr, &vendor_id, &product_id))
        {
            // Normal HID gamepad (Xbox controllers are handled at device level by xinput module)
            valid = pad_mount(idx, desc_report, desc_len, dev_addr, vendor_id, product_id);
            DBG("HID gamepad: VID=0x%04X, PID=0x%04X, valid=%d\n", vendor_id, product_id, valid);
        }
        else
        {
            DBG("Failed to get VID/PID for dev_addr %d\n", dev_addr);
        }
    }

    if (valid && !tuh_hid_receive_report(dev_addr, idx))
        DBG("tuh_hid_receive_report failed");
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    (void)dev_addr;
    hid_dev_addr[idx] = 0;
    pad_umount(idx);
}
