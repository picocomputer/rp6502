/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "sys/cfg.h"
#include "usb/kbd.h"
#include "usb/kbd_us.h"
#include "vga/term/ansi.h"
#include "pico/stdio/driver.h"
#include "fatfs/ff.h"

static int kbd_stdio_in_chars(char *buf, int length);

static stdio_driver_t kbd_stdio_app = {
    .in_chars = kbd_stdio_in_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = PICO_STDIO_DEFAULT_CRLF
#endif
};

#define KBD_REPEAT_DELAY 500000
#define KBD_REPEAT_RATE 30000
static absolute_time_t kbd_repeat_timer = {0};
static uint8_t kbd_repeat_keycode = 0;
static hid_keyboard_report_t kbd_prev_report = {0, 0, {0, 0, 0, 0, 0, 0}};
static char kbd_key_queue[8];
static uint8_t kbd_key_queue_in = 0;
static uint8_t kbd_key_queue_out = 0;

#define HID_KEYCODE_TO_UNICODE_(kb) HID_KEYCODE_TO_UNICODE_##kb
#define HID_KEYCODE_TO_UNICODE(kb) HID_KEYCODE_TO_UNICODE_(kb)
static DWORD const __in_flash("keycode_to_unicode")
    KEYCODE_TO_UNICODE[128][3] = {HID_KEYCODE_TO_UNICODE(RP6502_KEYBOARD)};

static void kbd_queue_key_str(const char *str)
{
    // TODO check for free space, change to head/tail style
    while (*str)
        kbd_key_queue[++kbd_key_queue_in & 7] = *str++;
}

static void kbd_queue_key(uint8_t modifier, uint8_t keycode, bool initial_press)
{
    kbd_repeat_keycode = keycode;
    kbd_repeat_timer = delayed_by_us(get_absolute_time(),
                                     initial_press ? KBD_REPEAT_DELAY : KBD_REPEAT_RATE);
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
        kbd_key_queue[++kbd_key_queue_in & 7] = ch;
        return;
    }
    if (initial_press)
        switch (keycode)
        {
        case HID_KEY_DELETE:
            if (modifier == (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTALT))
            {
                kbd_key_queue_out = kbd_key_queue_in;
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
        kbd_queue_key_str(ANSI_KEY_ARROW_RIGHT);
        break;
    case HID_KEY_ARROW_LEFT:
        kbd_queue_key_str(ANSI_KEY_ARROW_LEFT);
        break;
    case HID_KEY_DELETE:
        kbd_queue_key_str(ANSI_KEY_DELETE);
        break;
    }
}

static int kbd_stdio_in_chars(char *buf, int length)
{
    int i = 0;
    kbd_key_queue_in = kbd_key_queue_in & 7;
    if (kbd_key_queue_out > kbd_key_queue_in)
        kbd_key_queue_in += 8;
    while (i < length && kbd_key_queue_out < kbd_key_queue_in)
    {
        buf[i++] = kbd_key_queue[++kbd_key_queue_out & 7];
    }
    kbd_key_queue_out = kbd_key_queue_out & 7;
    return i ? i : PICO_ERROR_NO_DATA;
}

void kbd_report(uint8_t dev_addr, uint8_t instance, hid_keyboard_report_t const *report)
{
    static uint8_t prev_dev_addr = 0;
    static uint8_t prev_instance = 0;
    // Only support key presses on one keyboard at a time.
    if (kbd_prev_report.keycode[0] >= HID_KEY_A &&
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
                if (keycode == kbd_prev_report.keycode[j])
                    held = true;
            }
            if (!held)
                kbd_queue_key(modifier, keycode, true);
        }
    }
    prev_dev_addr = dev_addr;
    prev_instance = instance;
    kbd_prev_report = *report;
    kbd_prev_report.modifier = modifier;
}

void kbd_init()
{
    stdio_set_driver_enabled(&kbd_stdio_app, true);
}

void kbd_task()
{
    if (kbd_repeat_keycode && absolute_time_diff_us(get_absolute_time(), kbd_repeat_timer) < 0)
    {
        for (uint8_t i = 0; i < 6; i++)
        {
            uint8_t keycode = kbd_prev_report.keycode[5 - i];
            if (kbd_repeat_keycode == keycode)
            {
                kbd_queue_key(kbd_prev_report.modifier, keycode, false);
                return;
            }
        }
        kbd_repeat_keycode = 0;
    }
}

// TODO Retest. This works, but not through a USB hub.
//      It's too confusing without the LED.
// static void kbd_set_leds()
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
