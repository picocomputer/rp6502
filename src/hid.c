/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hid.h"
#include "ansi.h"
#include "tusb.h"
#include "pico/stdlib.h"
#include "pico/stdio/driver.h"

extern int process_sony_ds4(uint8_t dev_addr, uint8_t const *report, uint16_t len);
static int hid_stdio_in_chars(char *buf, int length);

static stdio_driver_t hid_stdio_app = {
    .in_chars = hid_stdio_in_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = PICO_STDIO_DEFAULT_CRLF
#endif
};

#define MAX_REPORT 4
static struct
{
    uint8_t report_count;
    tuh_hid_report_info_t report_info[MAX_REPORT];
} hid_info[CFG_TUH_HID];

static absolute_time_t key_repeat_timer = {0};
static hid_keyboard_report_t key_prev_report = {0, 0, {0}};
static char key_queue[8];
static uint8_t key_queue_in = 0;
static uint8_t key_queue_out = 0;

static void hid_queue_key_str(const char *str)
{
    while (*str)
        key_queue[++key_queue_in & 7] = *str++;
}

static void hid_queue_key(uint8_t modifier, uint8_t keycode, uint64_t repeat_delay_us)
{
    key_repeat_timer = delayed_by_us(get_absolute_time(), repeat_delay_us);
    modifier = ((modifier & 0xf0) >> 4) | (modifier & 0x0f); // merge modifiers to left
    static char const keycode_to_ascii[128][2] = {HID_KEYCODE_TO_ASCII};
    char ch = keycode_to_ascii[keycode][modifier & KEYBOARD_MODIFIER_LEFTSHIFT ? 1 : 0];
    if (modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_LEFTGUI))
        ch = 0;
    if (modifier & KEYBOARD_MODIFIER_LEFTCTRL)
    {
        if (ch >= '`' && ch <= '~')
            ch -= 96;
        else if (ch >= '@' && ch <= '_')
            ch -= 64;
        else
            ch = 0;
    }
    if (ch)
        key_queue[++key_queue_in & 7] = ch;
    else
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
    key_queue_in = key_queue_in & 7;
    if (key_queue_out > key_queue_in)
        key_queue_in += 8;
    while (i < length && key_queue_out < key_queue_in)
    {
        buf[i++] = key_queue[++key_queue_out & 7];
    }
    key_queue_out = key_queue_out & 7;
    return i ? i : PICO_ERROR_NO_DATA;
}

static void hid_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)dev_addr;

    uint8_t const rpt_count = hid_info[instance].report_count;
    tuh_hid_report_info_t *rpt_info_arr = hid_info[instance].report_info;
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
        printf("Couldn't find the report info for this report !\r\n");
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
    if (key_prev_report.keycode[0] && ((prev_dev_addr != dev_addr) || (prev_instance != instance)))
        return;
    for (uint8_t i = 0; i < 6; i++)
    {
        uint8_t keycode = report->keycode[i];
        if (keycode)
        {
            bool held = false;
            for (uint8_t j = 0; j < 6; j++)
            {
                if (keycode == key_prev_report.keycode[j])
                    held = true;
            }
            if (!held)
                hid_queue_key(report->modifier, keycode, 500000);
        }
    }
    prev_dev_addr = dev_addr;
    prev_instance = instance;
    key_prev_report = *report;
}

static void hid_mouse_report(hid_mouse_report_t const *report)
{
    printf("(%d %d %d) %c%c%c\n", report->x, report->y, report->wheel,
           report->buttons & MOUSE_BUTTON_LEFT ? 'L' : '-',
           report->buttons & MOUSE_BUTTON_MIDDLE ? 'M' : '-',
           report->buttons & MOUSE_BUTTON_RIGHT ? 'R' : '-');
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

    if (!tuh_hid_receive_report(dev_addr, instance))
    {
        printf("Error: tuh_hid_receive_report(%d, %d)\n", dev_addr, instance);
    }
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len)
{
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    if (itf_protocol == HID_ITF_PROTOCOL_NONE)
    {
        hid_info[instance].report_count =
            tuh_hid_parse_report_descriptor(hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);
    }

    const char *protocol_str[] = {"None", "Keyboard", "Mouse"};
    printf("HID mount: address = %d, instance = %d, ", dev_addr, instance);
    if (itf_protocol == HID_ITF_PROTOCOL_NONE)
        printf("reports = %u\n", hid_info[instance].report_count);
    else
        printf("protocol = %s\n", protocol_str[itf_protocol]);

    if (!tuh_hid_receive_report(dev_addr, instance))
    {
        printf("Error: tuh_hid_receive_report(%d, %d)\n", dev_addr, instance);
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    printf("HID unmount: address = %d, instance = %d, goodbye\n", dev_addr, instance);
}

void hid_init()
{
    stdio_set_driver_enabled(&hid_stdio_app, true);
}

void hid_task()
{
    absolute_time_t now = get_absolute_time();
    if (absolute_time_diff_us(now, key_repeat_timer) < 0)
    {
        for (uint8_t i = 0; i < 6; i++)
        {
            uint8_t keycode = key_prev_report.keycode[5 - i];
            if (keycode)
            {
                hid_queue_key(key_prev_report.modifier, keycode, 30000);
                return;
            }
        }
        key_repeat_timer = delayed_by_us(get_absolute_time(), 1000000);
    }
}

// This works, but not through a USB hub.
static void hid_set_scroll_lock()
{
    static uint8_t leds = KEYBOARD_LED_SCROLLLOCK;
    for (uint8_t dev_addr = 0; dev_addr < CFG_TUH_DEVICE_MAX; dev_addr++)
    {
        for (uint8_t inst = 0; inst < CFG_TUH_HID; inst++)
        {
            uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, inst);
            if (HID_ITF_PROTOCOL_KEYBOARD == itf_protocol)
            {
                tuh_hid_set_report(dev_addr, inst, 0, HID_REPORT_TYPE_OUTPUT, &leds, sizeof(leds));
            }
        }
    }
}
