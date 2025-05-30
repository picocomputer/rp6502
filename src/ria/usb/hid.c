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

static uint8_t hid_dev_addr[CFG_TUH_HID];

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *report, uint16_t len)
{
    (void)len;
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);
    switch (itf_protocol)
    {
    case HID_ITF_PROTOCOL_KEYBOARD:
        kbd_report(dev_addr, idx, (hid_keyboard_report_t const *)report);
        break;
    case HID_ITF_PROTOCOL_MOUSE:
        mou_report((hid_mouse_report_t const *)report);
        break;
    default:
        pad_report(dev_addr, report);
        break;
    }
    tuh_hid_receive_report(dev_addr, idx);
}

void hid_print_status(void)
{
    int count_keyboard = 0;
    int count_mouse = 0;
    int count_unspecified = 0;
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
                count_unspecified++;
                break;
            }
        }
    }
    printf("USB HID: %d keyboard%s, %d %s",
           count_keyboard, count_keyboard == 1 ? "" : "s",
           count_mouse, count_mouse == 1 ? "mouse" : "mice");
    if (count_unspecified)
        printf(", %d unspecified\n", count_unspecified);
    else
        printf("\n");
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *desc_report, uint16_t desc_len)
{
    (void)(desc_report);
    (void)(desc_len);
    hid_dev_addr[idx] = dev_addr;
    if (tuh_hid_interface_protocol(dev_addr, idx) == HID_ITF_PROTOCOL_KEYBOARD)
        kbd_hid_leds_dirty();
    tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    (void)(dev_addr);
    hid_dev_addr[idx] = 0;
}
