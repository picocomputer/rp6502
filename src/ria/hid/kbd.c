/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "btstack_hid_parser.h"
#include "main.h"
#include "api/api.h"
#include "sys/cfg.h"
#include "hid/kbd.h"
#include "hid/kbd_dan.h"
#include "hid/kbd_deu.h"
#include "hid/kbd_eng.h"
#include "hid/kbd_pol.h"
#include "hid/kbd_swe.h"
#include "usb/hid.h"
#include "fatfs/ff.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

// TODO SCROLLOCK

#define DEBUG_RIA_HID_KBD

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_KBD)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define KBD_REPEAT_DELAY 500000
#define KBD_REPEAT_RATE 30000

typedef struct
{
    uint8_t modifier;   /**< Keyboard modifier (KEYBOARD_MODIFIER_* masks). */
    uint8_t reserved;   /**< Reserved for OEM use, always set to 0. */
    uint8_t keycode[6]; /**< Key codes of the currently pressed keys. */
} hid_keyboard_report_t;

#define TU_BIT(n) (1UL << (n))

typedef enum
{
    KEYBOARD_MODIFIER_LEFTCTRL = TU_BIT(0),   ///< Left Control
    KEYBOARD_MODIFIER_LEFTSHIFT = TU_BIT(1),  ///< Left Shift
    KEYBOARD_MODIFIER_LEFTALT = TU_BIT(2),    ///< Left Alt
    KEYBOARD_MODIFIER_LEFTGUI = TU_BIT(3),    ///< Left Window
    KEYBOARD_MODIFIER_RIGHTCTRL = TU_BIT(4),  ///< Right Control
    KEYBOARD_MODIFIER_RIGHTSHIFT = TU_BIT(5), ///< Right Shift
    KEYBOARD_MODIFIER_RIGHTALT = TU_BIT(6),   ///< Right Alt
    KEYBOARD_MODIFIER_RIGHTGUI = TU_BIT(7)    ///< Right Window
} hid_keyboard_modifier_bm_t;

typedef enum
{
    KEYBOARD_LED_NUMLOCK = TU_BIT(0),   ///< Num Lock LED
    KEYBOARD_LED_CAPSLOCK = TU_BIT(1),  ///< Caps Lock LED
    KEYBOARD_LED_SCROLLLOCK = TU_BIT(2) ///< Scroll Lock LED
} hid_keyboard_led_bm_t;

#define HID_KEY_NONE 0x00
#define HID_KEY_A 0x04
#define HID_KEY_Z 0x1D
#define HID_KEY_BACKSPACE 0x2A
#define HID_KEY_CAPS_LOCK 0x39
#define HID_KEY_F1 0x3A
#define HID_KEY_F2 0x3B
#define HID_KEY_F3 0x3C
#define HID_KEY_F4 0x3D
#define HID_KEY_F5 0x3E
#define HID_KEY_F6 0x3F
#define HID_KEY_F7 0x40
#define HID_KEY_F8 0x41
#define HID_KEY_F9 0x42
#define HID_KEY_F10 0x43
#define HID_KEY_F11 0x44
#define HID_KEY_F12 0x45
#define HID_KEY_SCROLL_LOCK 0x47
#define HID_KEY_INSERT 0x49
#define HID_KEY_HOME 0x4A
#define HID_KEY_PAGE_UP 0x4B
#define HID_KEY_DELETE 0x4C
#define HID_KEY_END 0x4D
#define HID_KEY_PAGE_DOWN 0x4E
#define HID_KEY_ARROW_RIGHT 0x4F
#define HID_KEY_ARROW_LEFT 0x50
#define HID_KEY_ARROW_DOWN 0x51
#define HID_KEY_ARROW_UP 0x52
#define HID_KEY_NUM_LOCK 0x53
#define HID_KEY_KEYPAD_1 0x59
#define HID_KEY_KEYPAD_2 0x5A
#define HID_KEY_KEYPAD_3 0x5B
#define HID_KEY_KEYPAD_4 0x5C
#define HID_KEY_KEYPAD_5 0x5D
#define HID_KEY_KEYPAD_6 0x5E
#define HID_KEY_KEYPAD_7 0x5F
#define HID_KEY_KEYPAD_8 0x60
#define HID_KEY_KEYPAD_9 0x61
#define HID_KEY_KEYPAD_0 0x62
#define HID_KEY_KEYPAD_DECIMAL 0x63
#define HID_KEY_CONTROL_LEFT 0xE0
#define HID_KEY_SHIFT_LEFT 0xE1
#define HID_KEY_ALT_LEFT 0xE2
#define HID_KEY_GUI_LEFT 0xE3
#define HID_KEY_CONTROL_RIGHT 0xE4
#define HID_KEY_SHIFT_RIGHT 0xE5
#define HID_KEY_ALT_RIGHT 0xE6
#define HID_KEY_GUI_RIGHT 0xE7

static absolute_time_t kbd_repeat_timer;
static uint8_t kbd_repeat_keycode;
static char kbd_key_queue[16];
static uint8_t kbd_key_queue_head;
static uint8_t kbd_key_queue_tail;
static uint8_t kdb_hid_leds;

static uint8_t kbd_prev_report_idx;
static hid_keyboard_report_t kbd_prev_report;
static uint16_t kbd_xram;
static uint8_t kbd_xram_keys[32];

#define KBD_KEY_QUEUE(pos) kbd_key_queue[(pos) & 0x0F]

#define HID_KEYCODE_TO_UNICODE_(kb) HID_KEYCODE_TO_UNICODE_##kb
#define HID_KEYCODE_TO_UNICODE(kb) HID_KEYCODE_TO_UNICODE_(kb)
static DWORD const __in_flash("keycode_to_unicode")
    KEYCODE_TO_UNICODE[128][4] = {HID_KEYCODE_TO_UNICODE(RP6502_KEYBOARD)};

static void kbd_send_leds()
{
    hid_set_leds(kdb_hid_leds);
    // ble_set_leds(kdb_hid_leds);
}

static void kbd_queue_str(const char *str)
{
    // All or nothing
    for (size_t len = strlen(str); len; len--)
        if (&KBD_KEY_QUEUE(kbd_key_queue_head + len) == &KBD_KEY_QUEUE(kbd_key_queue_tail))
            return;
    while (*str)
        KBD_KEY_QUEUE(++kbd_key_queue_head) = *str++;
}

static void kbd_queue_seq(const char *str, const char *mod_seq, int mod)
{
    char s[16];
    if (mod == 1)
        return kbd_queue_str(str);
    sprintf(s, mod_seq, mod);
    return kbd_queue_str(s);
}

static void kbd_queue_seq_vt(int num, int mod)
{
    char s[16];
    if (mod == 1)
        sprintf(s, "\33[%d~", num);
    else
        sprintf(s, "\33[%d;%d~", num, mod);
    return kbd_queue_str(s);
}

static void kbd_queue_key(uint8_t modifier, uint8_t keycode, bool initial_press)
{
    bool key_shift = modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    bool key_alt = modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT);
    bool key_ctrl = modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL);
    bool key_gui = modifier & (KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI);
    bool is_numlock = kdb_hid_leds & KEYBOARD_LED_NUMLOCK;
    bool is_capslock = kdb_hid_leds & KEYBOARD_LED_CAPSLOCK;
    // Set up for repeat
    kbd_repeat_keycode = keycode;
    kbd_repeat_timer = delayed_by_us(get_absolute_time(),
                                     initial_press ? KBD_REPEAT_DELAY : KBD_REPEAT_RATE);
    // When not in numlock, and not shifted, remap num pad
    if (keycode >= HID_KEY_KEYPAD_1 &&
        keycode <= HID_KEY_KEYPAD_DECIMAL &&
        (!is_numlock || (key_shift && is_numlock)))
    {
        if (is_numlock)
            key_shift = false;
        switch (keycode)
        {
        case HID_KEY_KEYPAD_1:
            keycode = HID_KEY_END;
            break;
        case HID_KEY_KEYPAD_2:
            keycode = HID_KEY_ARROW_DOWN;
            break;
        case HID_KEY_KEYPAD_3:
            keycode = HID_KEY_PAGE_DOWN;
            break;
        case HID_KEY_KEYPAD_4:
            keycode = HID_KEY_ARROW_LEFT;
            break;
        case HID_KEY_KEYPAD_5:
            keycode = HID_KEY_NONE;
            break;
        case HID_KEY_KEYPAD_6:
            keycode = HID_KEY_ARROW_RIGHT;
            break;
        case HID_KEY_KEYPAD_7:
            keycode = HID_KEY_HOME;
            break;
        case HID_KEY_KEYPAD_8:
            keycode = HID_KEY_ARROW_UP;
            break;
        case HID_KEY_KEYPAD_9:
            keycode = HID_KEY_PAGE_UP;
            break;
        case HID_KEY_KEYPAD_0:
            keycode = HID_KEY_INSERT;
            break;
        case HID_KEY_KEYPAD_DECIMAL:
            keycode = HID_KEY_DELETE;
            break;
        }
    }
    // Find plain typed or AltGr character
    char ch = 0;
    if (keycode < 128 && !((modifier & (KEYBOARD_MODIFIER_LEFTALT |
                                        KEYBOARD_MODIFIER_LEFTGUI |
                                        KEYBOARD_MODIFIER_RIGHTGUI))))
    {
        bool use_shift = (key_shift && !is_capslock) ||
                         (key_shift && keycode > HID_KEY_Z) ||
                         (!key_shift && is_capslock && keycode <= HID_KEY_Z);
        if (modifier & KEYBOARD_MODIFIER_RIGHTALT)
        {
            if (use_shift)
                ch = ff_uni2oem(KEYCODE_TO_UNICODE[keycode][3], cfg_get_codepage());
            else
                ch = ff_uni2oem(KEYCODE_TO_UNICODE[keycode][2], cfg_get_codepage());
        }
        else
        {
            if (use_shift)
                ch = ff_uni2oem(KEYCODE_TO_UNICODE[keycode][1], cfg_get_codepage());
            else
                ch = ff_uni2oem(KEYCODE_TO_UNICODE[keycode][0], cfg_get_codepage());
        }
    }
    // ALT characters not found in AltGr get escaped
    if (key_alt && !ch && keycode < 128)
    {
        if (key_shift)
            ch = ff_uni2oem(KEYCODE_TO_UNICODE[keycode][1], cfg_get_codepage());
        else
            ch = ff_uni2oem(KEYCODE_TO_UNICODE[keycode][0], cfg_get_codepage());
        if (key_ctrl)
        {
            if (ch >= '`' && ch <= '~')
                ch -= 96;
            else if (ch >= '@' && ch <= '_')
                ch -= 64;
            else if (keycode == HID_KEY_BACKSPACE)
                ch = '\b';
        }
        if (ch)
        {
            if (&KBD_KEY_QUEUE(kbd_key_queue_head + 1) != &KBD_KEY_QUEUE(kbd_key_queue_tail) &&
                &KBD_KEY_QUEUE(kbd_key_queue_head + 2) != &KBD_KEY_QUEUE(kbd_key_queue_tail))
            {
                KBD_KEY_QUEUE(++kbd_key_queue_head) = '\33';
                KBD_KEY_QUEUE(++kbd_key_queue_head) = ch;
            }
            return;
        }
    }
    // Promote ctrl characters
    if (key_ctrl)
    {
        if (ch >= '`' && ch <= '~')
            ch -= 96;
        else if (ch >= '@' && ch <= '_')
            ch -= 64;
        else if (keycode == HID_KEY_BACKSPACE)
            ch = '\b';
        else
            ch = 0;
    }
    // Queue a regularly typed key
    if (ch)
    {
        if (&KBD_KEY_QUEUE(kbd_key_queue_head + 1) != &KBD_KEY_QUEUE(kbd_key_queue_tail))
            KBD_KEY_QUEUE(++kbd_key_queue_head) = ch;
        return;
    }
    // Non-repeating special key handler
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
        case HID_KEY_NUM_LOCK:
            kdb_hid_leds ^= KEYBOARD_LED_NUMLOCK;
            kbd_send_leds();
            break;
        case HID_KEY_CAPS_LOCK:
            kdb_hid_leds ^= KEYBOARD_LED_CAPSLOCK;
            kbd_send_leds();
            break;
        case HID_KEY_SCROLL_LOCK:
            kdb_hid_leds ^= KEYBOARD_LED_SCROLLLOCK;
            kbd_send_leds();
            break;
        }
    // Special key handler
    int ansi_modifier = 1;
    if (key_shift)
        ansi_modifier += 1;
    if (key_alt)
        ansi_modifier += 2;
    if (key_ctrl)
        ansi_modifier += 4;
    if (key_gui)
        ansi_modifier += 8;
    switch (keycode)
    {
    case HID_KEY_ARROW_UP:
        return kbd_queue_seq("\33[A", "\33[1;%dA", ansi_modifier);
    case HID_KEY_ARROW_DOWN:
        return kbd_queue_seq("\33[B", "\33[1;%dB", ansi_modifier);
    case HID_KEY_ARROW_RIGHT:
        return kbd_queue_seq("\33[C", "\33[1;%dC", ansi_modifier);
    case HID_KEY_ARROW_LEFT:
        return kbd_queue_seq("\33[D", "\33[1;%dD", ansi_modifier);
    case HID_KEY_F1:
        return kbd_queue_seq("\33OP", "\33[1;%dP", ansi_modifier);
    case HID_KEY_F2:
        return kbd_queue_seq("\33OQ", "\33[1;%dQ", ansi_modifier);
    case HID_KEY_F3:
        return kbd_queue_seq("\33OR", "\33[1;%dR", ansi_modifier);
    case HID_KEY_F4:
        return kbd_queue_seq("\33OS", "\33[1;%dS", ansi_modifier);
    case HID_KEY_F5:
        return kbd_queue_seq_vt(15, ansi_modifier);
    case HID_KEY_F6:
        return kbd_queue_seq_vt(17, ansi_modifier);
    case HID_KEY_F7:
        return kbd_queue_seq_vt(18, ansi_modifier);
    case HID_KEY_F8:
        return kbd_queue_seq_vt(19, ansi_modifier);
    case HID_KEY_F9:
        return kbd_queue_seq_vt(10, ansi_modifier);
    case HID_KEY_F10:
        return kbd_queue_seq_vt(21, ansi_modifier);
    case HID_KEY_F11:
        return kbd_queue_seq_vt(23, ansi_modifier);
    case HID_KEY_F12:
        return kbd_queue_seq_vt(24, ansi_modifier);
    case HID_KEY_HOME:
        return kbd_queue_seq("\33[H", "\33[1;%dH", ansi_modifier);
    case HID_KEY_INSERT:
        return kbd_queue_seq_vt(2, ansi_modifier);
    case HID_KEY_DELETE:
        return kbd_queue_seq_vt(3, ansi_modifier);
    case HID_KEY_END:
        return kbd_queue_seq("\33[F", "\33[1;%dF", ansi_modifier);
    case HID_KEY_PAGE_UP:
        return kbd_queue_seq_vt(5, ansi_modifier);
    case HID_KEY_PAGE_DOWN:
        return kbd_queue_seq_vt(6, ansi_modifier);
    }
}

int kbd_stdio_in_chars(char *buf, int length)
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

        // No key pressed
        if (!any_key && !kbd_prev_report.modifier && !phantom)
            kbd_xram_keys[0] |= 1;

        // NUMLOCK CAPSLOCK SCROLLLOCK
        kbd_xram_keys[0] |= (kdb_hid_leds & 7) << 1;

        // Send it to xram
        memcpy(&xram[kbd_xram], kbd_xram_keys, sizeof(kbd_xram_keys));
    }
}

void kbd_report(uint8_t slot, void const *report_ptr, size_t size)
{
    if (size < sizeof(hid_keyboard_report_t))
        return;

    hid_keyboard_report_t const *report = report_ptr;

    // Only support key presses on one keyboard at a time.
    if (kbd_prev_report.keycode[0] >= HID_KEY_A && kbd_prev_report_idx != slot)
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
    kbd_prev_report_idx = slot;
    kbd_prev_report = *report;
    kbd_prev_report.modifier = modifier;
    kbd_prev_report_to_xram();
}

void kbd_init(void)
{
    kbd_stop();
    kdb_hid_leds = KEYBOARD_LED_NUMLOCK;
    kbd_send_leds();
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
