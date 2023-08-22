/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "tusb.h"
#include "sys/cfg.h"
#include "usb/hid.h"
#include "usb/kbd_en.h"
#include "usb/usb.h"
#include "vga/term/ansi.h"
#include "pico/stdio/driver.h"
#include "fatfs/ff.h"

extern int process_sony_ds4(uint8_t dev_addr, uint8_t const *report, uint16_t len);
static int hid_stdio_in_chars(char *buf, int length);

static stdio_driver_t hid_stdio_app = {
    .in_chars = hid_stdio_in_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = PICO_STDIO_DEFAULT_CRLF
#endif
};

#define HID_MAX_REPORT 4
static struct hid_info
{
    uint8_t report_count;
    tuh_hid_report_info_t report_info[HID_MAX_REPORT];
} hid_info[CFG_TUH_DEVICE_MAX][CFG_TUH_HID];

#define HID_REPEAT_DELAY 500000
#define HID_REPEAT_RATE 30000
static absolute_time_t hid_repeat_timer = {0};
static uint8_t hid_repeat_keycode = 0;
static hid_keyboard_report_t hid_prev_report = {0, 0, {0, 0, 0, 0, 0, 0}};
static char hid_key_queue[8];
static uint8_t hid_key_queue_in = 0;
static uint8_t hid_key_queue_out = 0;

static DWORD const __in_flash("keycode_to_ascii")
    KEYCODE_TO_UNICODE[128][3] = {HID_KEYCODE_TO_UNICODE_EN};

static void hid_queue_key_str(const char *str)
{
    while (*str)
        hid_key_queue[++hid_key_queue_in & 7] = *str++;
}

static void hid_queue_key(uint8_t modifier, uint8_t keycode, bool initial_press)
{
    hid_repeat_keycode = keycode;
    hid_repeat_timer = delayed_by_us(get_absolute_time(),
                                     initial_press ? HID_REPEAT_DELAY : HID_REPEAT_RATE);
    char ch = 0;
    if (keycode < 128 && !((modifier & (KEYBOARD_MODIFIER_LEFTALT |
                                        KEYBOARD_MODIFIER_LEFTGUI |
                                        KEYBOARD_MODIFIER_RIGHTGUI))))
    {
        if (modifier & (KEYBOARD_MODIFIER_RIGHTALT))
        {
            ch = ff_uni2oem(KEYCODE_TO_UNICODE[keycode][2], cfg_get_codepage());
        }
        else if (modifier & (KEYBOARD_MODIFIER_LEFTSHIFT |
                             KEYBOARD_MODIFIER_RIGHTSHIFT))
        {
            ch = ff_uni2oem(KEYCODE_TO_UNICODE[keycode][1], cfg_get_codepage());
        }
        else
            ch = ff_uni2oem(KEYCODE_TO_UNICODE[keycode][0], cfg_get_codepage());
    }
    if (modifier & (KEYBOARD_MODIFIER_LEFTCTRL |
                    KEYBOARD_MODIFIER_RIGHTCTRL))
    {
        if (ch >= '`' && ch <= '~')
            ch -= 96;
        else if (ch >= '@' && ch <= '_')
            ch -= 64;
        else
            ch = 0;
    }
    if (ch)
    {
        hid_key_queue[++hid_key_queue_in & 7] = ch;
        return;
    }
    if (initial_press)
        switch (keycode)
        {
        case HID_KEY_DELETE:
            if (modifier == (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTALT))
            {
                hid_key_queue_out = hid_key_queue_in;
                main_break();
            }
            break;
        case HID_KEY_CAPS_LOCK:
            // TODO
            break;
        }
    switch (keycode)
    {
    case HID_KEY_ARROW_RIGHT:
        hid_queue_key_str(ANSI_KEY_ARROW_RIGHT);
        break;
    case HID_KEY_ARROW_LEFT:
        hid_queue_key_str(ANSI_KEY_ARROW_LEFT);
        break;
    case HID_KEY_DELETE:
        hid_queue_key_str(ANSI_KEY_DELETE);
        break;
    }
}

static int hid_stdio_in_chars(char *buf, int length)
{
    int i = 0;
    hid_key_queue_in = hid_key_queue_in & 7;
    if (hid_key_queue_out > hid_key_queue_in)
        hid_key_queue_in += 8;
    while (i < length && hid_key_queue_out < hid_key_queue_in)
    {
        buf[i++] = hid_key_queue[++hid_key_queue_out & 7];
    }
    hid_key_queue_out = hid_key_queue_out & 7;
    return i ? i : PICO_ERROR_NO_DATA;
}

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

static void hid_kbd_report(uint8_t dev_addr, uint8_t instance, hid_keyboard_report_t const *report)
{
    static uint8_t prev_dev_addr = 0;
    static uint8_t prev_instance = 0;
    // Only support key presses on one keyboard at a time.
    if (hid_prev_report.keycode[0] >= HID_KEY_A &&
        ((prev_dev_addr != dev_addr) || (prev_instance != instance)))
        return;
    uint8_t modifier = report->modifier;
    for (uint8_t i = 0; i < 6; i++)
    {
        uint8_t keycode = report->keycode[i];
        if (keycode >= HID_KEY_CONTROL_LEFT && keycode <= HID_KEY_GUI_RIGHT)
            modifier |= 1 << (keycode & 7);
    }
    for (uint8_t i = 0; i < 6; i++)
    {
        uint8_t keycode = report->keycode[i];
        if (keycode >= HID_KEY_A &&
            !(keycode >= HID_KEY_CONTROL_LEFT && keycode <= HID_KEY_GUI_RIGHT))
        {
            bool held = false;
            for (uint8_t j = 0; j < 6; j++)
            {
                if (keycode == hid_prev_report.keycode[j])
                    held = true;
            }
            if (!held)
                hid_queue_key(modifier, keycode, true);
        }
    }
    prev_dev_addr = dev_addr;
    prev_instance = instance;
    hid_prev_report = *report;
    hid_prev_report.modifier = modifier;
}

static void hid_mouse_report(hid_mouse_report_t const *report)
{
    printf("(%d %d %d) %c%c%c\n", report->x, report->y, report->wheel,
           report->buttons & MOUSE_BUTTON_LEFT ? 'L' : '-',
           report->buttons & MOUSE_BUTTON_MIDDLE ? 'M' : '-',
           report->buttons & MOUSE_BUTTON_RIGHT ? 'R' : '-');
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
        hid_kbd_report(dev_addr, instance, (hid_keyboard_report_t const *)report);
        break;

    case HID_ITF_PROTOCOL_MOUSE:
        hid_mouse_report((hid_mouse_report_t const *)report);
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

void hid_init()
{
    stdio_set_driver_enabled(&hid_stdio_app, true);
}

void hid_task()
{
    if (hid_repeat_keycode && absolute_time_diff_us(get_absolute_time(), hid_repeat_timer) < 0)
    {
        for (uint8_t i = 0; i < 6; i++)
        {
            uint8_t keycode = hid_prev_report.keycode[5 - i];
            if (hid_repeat_keycode == keycode)
            {
                hid_queue_key(hid_prev_report.modifier, keycode, false);
                return;
            }
        }
        hid_repeat_keycode = 0;
    }
}

// TODO retest. This works, but not through a USB hub.
// static void hid_set_leds()
// {
//     static uint8_t hid_leds = KEYBOARD_LED_SCROLLLOCK;
//     for (uint8_t dev_addr = 0; dev_addr < CFG_TUH_DEVICE_MAX; dev_addr++)
//     {
//         for (uint8_t inst = 0; inst < CFG_TUH_HID; inst++)
//         {
//             uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, inst);
//             if (HID_ITF_PROTOCOL_KEYBOARD == itf_protocol)
//             {
//                 tuh_hid_set_report(dev_addr, inst, 0, HID_REPORT_TYPE_OUTPUT, &hid_leds, sizeof(hid_leds));
//             }
//         }
//     }
// }
