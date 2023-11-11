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
#include "pico/stdio/driver.h"
#include "fatfs/ff.h"
#include "string.h"

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
static char kbd_key_queue[16];
static uint8_t kbd_key_queue_head;
static uint8_t kbd_key_queue_tail;
static uint8_t kdb_hid_leds = KEYBOARD_LED_NUMLOCK;
static bool kdb_hid_leds_need_report;
static uint16_t kbd_xram;
static uint8_t kbd_xram_keys[32];

#define KBD_KEY_QUEUE(pos) kbd_key_queue[(pos) & 0x0F]

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
    // All or nothing
    for (size_t len = strlen(str); len; len--)
        if (&KBD_KEY_QUEUE(kbd_key_queue_head + len) == &KBD_KEY_QUEUE(kbd_key_queue_tail))
            return;
    while (*str)
        KBD_KEY_QUEUE(++kbd_key_queue_head) = *str++;
}

static void kbd_queue_key_seq(const char *str, const char *mod_seq, int mod)
{
    char s[16];
    if (mod == 1)
        return kbd_queue_key_str(str);
    sprintf(s, mod_seq, mod);
    return kbd_queue_key_str(s);
}

static void kbd_queue_key(uint8_t modifier, uint8_t keycode, bool initial_press)
{
    bool key_ctrl = modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL);
    bool key_alt = modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT);
    bool key_shift = modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    bool key_meta = modifier & (KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI);
    kbd_repeat_keycode = keycode;
    kbd_repeat_timer = delayed_by_us(get_absolute_time(),
                                     initial_press ? KBD_REPEAT_DELAY : KBD_REPEAT_RATE);
    char ch = 0;
    if (keycode < 128 && !((modifier & (KEYBOARD_MODIFIER_LEFTALT |
                                        KEYBOARD_MODIFIER_LEFTGUI |
                                        KEYBOARD_MODIFIER_RIGHTGUI))))
    {
        bool is_caps_lock = kdb_hid_leds & KEYBOARD_LED_CAPSLOCK;
        if (modifier & KEYBOARD_MODIFIER_RIGHTALT)
        {
            ch = ff_uni2oem(KEYCODE_TO_UNICODE[keycode][2], cfg_get_codepage());
        }
        else if ((key_shift && !is_caps_lock) || (!key_shift && is_caps_lock))
        {
            ch = ff_uni2oem(KEYCODE_TO_UNICODE[keycode][1], cfg_get_codepage());
        }
        else
            ch = ff_uni2oem(KEYCODE_TO_UNICODE[keycode][0], cfg_get_codepage());
    }
    if (key_ctrl)
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
        if (&KBD_KEY_QUEUE(kbd_key_queue_head + 1) != &KBD_KEY_QUEUE(kbd_key_queue_tail))
            KBD_KEY_QUEUE(++kbd_key_queue_head) = ch;
        return;
    }
    if (initial_press)
        switch (keycode)
        {
        case HID_KEY_DELETE:
            if (key_ctrl && key_alt)
            {
                kbd_key_queue_tail = kbd_key_queue_head;
                main_break();
                return;
            }
            break;
        case HID_KEY_CAPS_LOCK:
            kdb_hid_leds ^= KEYBOARD_LED_CAPSLOCK;
            kbd_hid_leds_dirty();
            break;
        }
    int ansi_modifier = 1;
    if (key_shift)
        ansi_modifier += 1;
    if (key_alt)
        ansi_modifier += 2;
    if (key_ctrl)
        ansi_modifier += 4;
    if (key_meta)
        ansi_modifier += 8;
    switch (keycode)
    {
    case HID_KEY_ARROW_UP:
        return kbd_queue_key_seq("\33[A", "\33[1;%dA", ansi_modifier);
    case HID_KEY_ARROW_DOWN:
        return kbd_queue_key_seq("\33[B", "\33[1;%dB", ansi_modifier);
    case HID_KEY_ARROW_RIGHT:
        return kbd_queue_key_seq("\33[C", "\33[1;%dC", ansi_modifier);
    case HID_KEY_ARROW_LEFT:
        return kbd_queue_key_seq("\33[D", "\33[1;%dD", ansi_modifier);
    case HID_KEY_DELETE:
        return kbd_queue_key_str("\33\x7F");
    }
}

static int kbd_stdio_in_chars(char *buf, int length)
{
    int i = 0;
    while (i < length && &KBD_KEY_QUEUE(kbd_key_queue_tail) != &KBD_KEY_QUEUE(kbd_key_queue_head))
    {
        buf[i++] = KBD_KEY_QUEUE(++kbd_key_queue_tail);
    }
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

bool kbd_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - sizeof(kbd_xram_keys))
        return false;
    kbd_xram = word;
    kbd_prev_report_to_xram();
    return true;
}
