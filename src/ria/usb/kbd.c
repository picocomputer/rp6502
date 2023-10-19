/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "sys/cfg.h"
#include "usb/kbd.h"
#include "usb/kbd_deu.h"
#include "usb/kbd_eng.h"
#include "usb/kbd_swe.h"
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
static absolute_time_t kbd_repeat_timer;
static uint8_t kbd_repeat_keycode;
static hid_keyboard_report_t kbd_prev_report;
static char kbd_key_queue[8];
static uint8_t kbd_key_queue_head;
static uint8_t kbd_key_queue_tail;
static uint8_t kdb_hid_leds = KEYBOARD_LED_NUMLOCK;
static bool kdb_hid_leds_need_report;
static uint16_t kbd_xram;
static uint8_t kbd_xram_keys[32];

#define HID_KEYCODE_TO_UNICODE_(kb) HID_KEYCODE_TO_UNICODE_##kb
#define HID_KEYCODE_TO_UNICODE(kb) HID_KEYCODE_TO_UNICODE_(kb)
static DWORD const __in_flash("keycode_to_unicode")
    KEYCODE_TO_UNICODE[128][3] = {HID_KEYCODE_TO_UNICODE(RP6502_KEYBOARD)};

void kbd_hid_leds_dirty()
{
    kdb_hid_leds_need_report = true;
}

static void kbd_queue_key_str(const char *str)
{
    for (size_t len = strlen(str); len; len--)
        if (&kbd_key_queue[(kbd_key_queue_head + len) & 7] == &kbd_key_queue[kbd_key_queue_tail & 7])
            return;
    while (*str)
        kbd_key_queue[++kbd_key_queue_head & 7] = *str++;
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
        bool is_alt_gr = modifier & (KEYBOARD_MODIFIER_RIGHTALT);
        bool is_shift = modifier & (KEYBOARD_MODIFIER_LEFTSHIFT |
                                    KEYBOARD_MODIFIER_RIGHTSHIFT);
        bool is_caps_lock = kdb_hid_leds & KEYBOARD_LED_CAPSLOCK;
        if (is_alt_gr)
        {
            ch = ff_uni2oem(KEYCODE_TO_UNICODE[keycode][2], cfg_get_codepage());
        }
        else if ((is_shift && !is_caps_lock) || (!is_shift && is_caps_lock))
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
        kbd_key_queue[++kbd_key_queue_head & 7] = ch;
        return;
    }
    if (initial_press)
        switch (keycode)
        {
        case HID_KEY_DELETE:
            if ((modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) &&
                (modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)))
            {
                kbd_key_queue_tail = kbd_key_queue_head;
                main_break();
            }
            break;
        case HID_KEY_CAPS_LOCK:
            kdb_hid_leds ^= KEYBOARD_LED_CAPSLOCK;
            kbd_hid_leds_dirty();
            break;
        }
    switch (keycode)
    {
    case HID_KEY_ARROW_UP:
        kbd_queue_key_str(ANSI_KEY_ARROW_UP);
        break;
    case HID_KEY_ARROW_DOWN:
        kbd_queue_key_str(ANSI_KEY_ARROW_DOWN);
        break;
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
    kbd_key_queue_head = kbd_key_queue_head & 7;
    if (kbd_key_queue_tail > kbd_key_queue_head)
        kbd_key_queue_head += 8;
    while (i < length && kbd_key_queue_tail < kbd_key_queue_head)
    {
        buf[i++] = kbd_key_queue[++kbd_key_queue_tail & 7];
    }
    kbd_key_queue_tail = kbd_key_queue_tail & 7;
    return i ? i : PICO_ERROR_NO_DATA;
}

static void kbd_prev_report_to_xram()
{
    // Update xram if configured
    if (kbd_xram != 0xFFFF)
    {
        // Check for phantom state
        bool phantom = false;
        for (uint8_t i = 0; i < 6; i++)
            if (kbd_prev_report.keycode[i] == 1)
                phantom = true;
        // Preserve previous keys in phantom state
        if (!phantom)
            memset(kbd_xram_keys, 0, sizeof(kbd_xram_keys));
        bool any_key = false;
        for (uint8_t i = 0; i < 6; i++)
        {
            uint8_t keycode = kbd_prev_report.keycode[i];
            if (keycode >= HID_KEY_A)
            {
                any_key = true;
                kbd_xram_keys[keycode >> 3] |= 1 << (keycode & 7);
            }
        }
        // modifier maps directly
        kbd_xram_keys[HID_KEY_CONTROL_LEFT >> 3] = kbd_prev_report.modifier;
        // The Any Key
        if (!any_key && !kbd_prev_report.modifier && !phantom)
            kbd_xram_keys[0] |= 1;
        // Send it to xram
        memcpy(&xram[kbd_xram], kbd_xram_keys, sizeof(kbd_xram_keys));
    }
}

void kbd_report(uint8_t dev_addr, uint8_t instance, hid_keyboard_report_t const *report)
{
    static uint8_t prev_dev_addr = 0;
    static uint8_t prev_instance = 0;
    // Only support key presses on one keyboard at a time.
    if (kbd_prev_report.keycode[0] >= HID_KEY_A &&
        ((prev_dev_addr != dev_addr) || (prev_instance != instance)))
        return;

    // Extract presses for queue
    uint8_t modifier = report->modifier;
    for (uint8_t i = 0; i < 6; i++)
    {
        // fix unusual modifier reports
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
    kbd_prev_report_to_xram();
}

void kbd_init(void)
{
    stdio_set_driver_enabled(&kbd_stdio_app, true);
    kbd_stop();
}

void kbd_task(void)
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

    if (kdb_hid_leds_need_report)
    {
        kdb_hid_leds_need_report = false;
        for (uint8_t dev_addr = 0; dev_addr < CFG_TUH_DEVICE_MAX; dev_addr++)
            for (uint8_t instance = 0; instance < CFG_TUH_HID; instance++)
                if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD)
                    tuh_hid_set_report(dev_addr, instance, 0, HID_REPORT_TYPE_OUTPUT,
                                       &kdb_hid_leds, sizeof(kdb_hid_leds));
    }
}

void kbd_stop(void)
{
    kbd_xram = 0xFFFF;
}

bool kbd_pix(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - sizeof(kbd_xram_keys))
        return false;
    kbd_xram = word;
    kbd_prev_report_to_xram();
    return true;
}
