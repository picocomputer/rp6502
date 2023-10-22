/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb.h"
#include "usb/kbd.h"
#include "usb/mou.h"
#include "usb/usb.h"

// TODO this is temporary, need full and proper gamepad support
extern int process_sony_ds4(uint8_t dev_addr, uint8_t const *report, uint16_t len);

#define HID_MAX_REPORT 4
static struct hid_info
{
    uint8_t report_count;
    tuh_hid_report_info_t report_info[HID_MAX_REPORT];
} hid_info[CFG_TUH_DEVICE_MAX][CFG_TUH_HID];

static void hid_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)dev_addr;

    uint8_t const rpt_count = hid_info[dev_addr][instance].report_count;
    tuh_hid_report_info_t *rpt_info_arr = hid_info[dev_addr][instance].report_info;
    tuh_hid_report_info_t *rpt_info = NULL;

    if (rpt_count == 1 && rpt_info_arr[0].report_id == 0)
    {
        rpt_info = &rpt_info_arr[0];
    }
    else
    {
        uint8_t const rpt_id = report[0];
        for (uint8_t i = 0; i < rpt_count; i++)
        {
            if (rpt_id == rpt_info_arr[i].report_id)
            {
                rpt_info = &rpt_info_arr[i];
                break;
            }
        }
        report++;
        len--;
    }

    if (!rpt_info)
    {
        // printf("Couldn't find the report info for this report !\r\n");
        return;
    }

    if (rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP)
    {
        switch (rpt_info->usage)
        {
        case HID_USAGE_DESKTOP_JOYSTICK:
            printf("HID receive joystick report\n");
            break;
        case HID_USAGE_DESKTOP_GAMEPAD:
            printf("HID receive gamepad report\n");
            break;
        default:
            break;
        }
    }
}

static bool hid_receive_report(uint8_t dev_addr, uint8_t instance)
{
    if (tuh_hid_receive_report(dev_addr, instance))
        return true;
    usb_set_status(dev_addr, "?HID unable to receive report on devce %d instance %d", dev_addr, instance);
    return false;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len)
{
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    switch (itf_protocol)
    {
    case HID_ITF_PROTOCOL_KEYBOARD:
        kbd_report(dev_addr, instance, (hid_keyboard_report_t const *)report);
        break;

    case HID_ITF_PROTOCOL_MOUSE:
        mou_report((hid_mouse_report_t const *)report);
        break;

    default:
        if (process_sony_ds4(dev_addr, report, len))
            hid_generic_report(dev_addr, instance, report, len);
    }

    hid_receive_report(dev_addr, instance);
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len)
{
    struct hid_info dev_hid_info = hid_info[dev_addr][instance];
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_NONE)
    {
        dev_hid_info.report_count =
            tuh_hid_parse_report_descriptor(
                dev_hid_info.report_info,
                HID_MAX_REPORT, desc_report, desc_len);
    }

    bool has_keyboard = false;
    bool has_mouse = false;
    uint8_t other_reports = 0;

    for (int i = 0; i <= instance; i++)
    {
        switch (tuh_hid_interface_protocol(dev_addr, i))
        {
        case HID_ITF_PROTOCOL_KEYBOARD:
            has_keyboard = true;
            kbd_hid_leds_dirty();
            break;
        case HID_ITF_PROTOCOL_MOUSE:
            has_mouse = true;
            break;
        case HID_ITF_PROTOCOL_NONE:
            other_reports++;
            break;
        }
    }

    if (has_keyboard && has_mouse && other_reports)
        usb_set_status(dev_addr, "HID keyboard, mouse, and %d other reports", other_reports);
    else if (has_keyboard && other_reports)
        usb_set_status(dev_addr, "HID keyboard and %d other reports", other_reports);
    else if (has_mouse && other_reports)
        usb_set_status(dev_addr, "HID mouse and %d other reports", other_reports);
    else if (has_keyboard && has_mouse)
        usb_set_status(dev_addr, "HID keyboard and mouse");
    else if (has_keyboard)
        usb_set_status(dev_addr, "HID keyboard");
    else if (has_mouse)
        usb_set_status(dev_addr, "HID mouse");
    else
        usb_set_status(dev_addr, "HID %d report%s", other_reports, other_reports == 1 ? "" : "s");

    hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    (void)(dev_addr);
    (void)(instance);
}
