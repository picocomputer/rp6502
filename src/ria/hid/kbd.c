/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "hid/kbd.h"
#include "hid/kbd_dan.h"
#include "hid/kbd_deu.h"
#include "hid/kbd_eng.h"
#include "hid/kbd_pol.h"
#include "hid/kbd_swe.h"
#include "hid/des.h"
#include "net/ble.h"
#include "sys/cfg.h"
#include "usb/hid.h"
#include <btstack_hid_parser.h>
#include <fatfs/ff.h>
#include <pico/time.h>
#include <stdio.h>

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_KBD)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// These usually come from TinyUSB's hid.h but we can't
// include that while using btstack_hid_parser.h.
#define KBD_HID_KEY_NONE 0x00
#define KBD_HID_KEY_A 0x04
#define KBD_HID_KEY_Z 0x1D
#define KBD_HID_KEY_BACKSPACE 0x2A
#define KBD_HID_KEY_CAPS_LOCK 0x39
#define KBD_HID_KEY_F1 0x3A
#define KBD_HID_KEY_F2 0x3B
#define KBD_HID_KEY_F3 0x3C
#define KBD_HID_KEY_F4 0x3D
#define KBD_HID_KEY_F5 0x3E
#define KBD_HID_KEY_F6 0x3F
#define KBD_HID_KEY_F7 0x40
#define KBD_HID_KEY_F8 0x41
#define KBD_HID_KEY_F9 0x42
#define KBD_HID_KEY_F10 0x43
#define KBD_HID_KEY_F11 0x44
#define KBD_HID_KEY_F12 0x45
#define KBD_HID_KEY_SCROLL_LOCK 0x47
#define KBD_HID_KEY_INSERT 0x49
#define KBD_HID_KEY_HOME 0x4A
#define KBD_HID_KEY_PAGE_UP 0x4B
#define KBD_HID_KEY_DELETE 0x4C
#define KBD_HID_KEY_END 0x4D
#define KBD_HID_KEY_PAGE_DOWN 0x4E
#define KBD_HID_KEY_ARROW_RIGHT 0x4F
#define KBD_HID_KEY_ARROW_LEFT 0x50
#define KBD_HID_KEY_ARROW_DOWN 0x51
#define KBD_HID_KEY_ARROW_UP 0x52
#define KBD_HID_KEY_NUM_LOCK 0x53
#define KBD_HID_KEY_KEYPAD_1 0x59
#define KBD_HID_KEY_KEYPAD_2 0x5A
#define KBD_HID_KEY_KEYPAD_3 0x5B
#define KBD_HID_KEY_KEYPAD_4 0x5C
#define KBD_HID_KEY_KEYPAD_5 0x5D
#define KBD_HID_KEY_KEYPAD_6 0x5E
#define KBD_HID_KEY_KEYPAD_7 0x5F
#define KBD_HID_KEY_KEYPAD_8 0x60
#define KBD_HID_KEY_KEYPAD_9 0x61
#define KBD_HID_KEY_KEYPAD_0 0x62
#define KBD_HID_KEY_KEYPAD_DECIMAL 0x63
#define KBD_HID_KEY_CONTROL_LEFT 0xE0
#define KBD_HID_KEY_SHIFT_LEFT 0xE1
#define KBD_HID_KEY_ALT_LEFT 0xE2
#define KBD_HID_KEY_GUI_LEFT 0xE3
#define KBD_HID_KEY_CONTROL_RIGHT 0xE4
#define KBD_HID_KEY_SHIFT_RIGHT 0xE5
#define KBD_HID_KEY_ALT_RIGHT 0xE6
#define KBD_HID_KEY_GUI_RIGHT 0xE7

#define KBD_MODIFIER_LEFTCTRL 1 << 0   // Left Control
#define KBD_MODIFIER_LEFTSHIFT 1 << 1  // Left Shift
#define KBD_MODIFIER_LEFTALT 1 << 2    // Left Alt
#define KBD_MODIFIER_LEFTGUI 1 << 3    // Left Window
#define KBD_MODIFIER_RIGHTCTRL 1 << 4  // Right Control
#define KBD_MODIFIER_RIGHTSHIFT 1 << 5 // Right Shift
#define KBD_MODIFIER_RIGHTALT 1 << 6   // Right Alt
#define KBD_MODIFIER_RIGHTGUI 1 << 7   // Right Window

#define KBD_LED_NUMLOCK 1 << 0    // Num Lock LED
#define KBD_LED_CAPSLOCK 1 << 1   // Caps Lock LED
#define KBD_LED_SCROLLLOCK 1 << 2 // Scroll Lock LED

#define KBD_REPEAT_DELAY 500000
#define KBD_REPEAT_RATE 30000

static absolute_time_t kbd_repeat_timer;
static uint8_t kbd_repeat_modifier;
static uint8_t kbd_repeat_keycode;
static char kbd_key_queue[16];
static uint8_t kbd_key_queue_head;
static uint8_t kbd_key_queue_tail;
static uint8_t kdb_hid_leds;
static uint16_t kbd_xram;
static uint32_t kbd_keys[8];

typedef struct
{
    bool valid;
    int slot;               // HID slot
    uint32_t keys[8];       // last report, bits 0-3 unused
    uint8_t report_id;      // If non zero, the first report byte must match and will be skipped
    uint16_t codes_offset;  // Offset in bits for keycode array
    uint8_t codes_count;    // Number of keycodes in array
    uint16_t keycodes[256]; // Offsets of all bitmap keys
} kbd_connection_t;

#define KBD_MAX_KEYBOARDS 4
static kbd_connection_t kbd_connections[KBD_MAX_KEYBOARDS];

#define KBD_KEY_BIT_SET(data, keycode) (data[keycode >> 5] |= 1 << (keycode & 31))
#define KBD_KEY_BIT_VAL(data, keycode) (data[keycode >> 5] & (1 << (keycode & 31)))

// Direct access to modifier byte of a kbd_connection_t.keys
#define KBD_MODIFIER(keys) ((uint8_t *)keys)[KBD_HID_KEY_CONTROL_LEFT >> 3]

// Circular buffer
#define KBD_KEY_QUEUE(pos) kbd_key_queue[(pos) & 0x0F]

// Select locale based on RP6502_KEYBOARD set in CMakeLists.txt
#define HID_KEYCODE_TO_UNICODE_(kb) HID_KEYCODE_TO_UNICODE_##kb
#define HID_KEYCODE_TO_UNICODE(kb) HID_KEYCODE_TO_UNICODE_(kb)
static DWORD const __in_flash("keycode_to_unicode")
    KEYCODE_TO_UNICODE[128][4] = {HID_KEYCODE_TO_UNICODE(RP6502_KEYBOARD)};

static kbd_connection_t *kbd_get_connection_by_slot(int slot)
{
    for (int i = 0; i < KBD_MAX_KEYBOARDS; i++)
        if (kbd_connections[i].valid && kbd_connections[i].slot == slot)
            return &kbd_connections[i];
    return NULL;
}

static void kbd_send_leds()
{
    hid_set_leds(kdb_hid_leds);
    ble_set_leds(kdb_hid_leds);
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
    bool key_shift = modifier & (KBD_MODIFIER_LEFTSHIFT | KBD_MODIFIER_RIGHTSHIFT);
    bool key_alt = modifier & (KBD_MODIFIER_LEFTALT | KBD_MODIFIER_RIGHTALT);
    bool key_ctrl = modifier & (KBD_MODIFIER_LEFTCTRL | KBD_MODIFIER_RIGHTCTRL);
    bool key_gui = modifier & (KBD_MODIFIER_LEFTGUI | KBD_MODIFIER_RIGHTGUI);
    bool is_numlock = kdb_hid_leds & KBD_LED_NUMLOCK;
    bool is_capslock = kdb_hid_leds & KBD_LED_CAPSLOCK;
    // Set up for repeat
    kbd_repeat_modifier = modifier;
    kbd_repeat_keycode = keycode;
    kbd_repeat_timer = delayed_by_us(get_absolute_time(),
                                     initial_press ? KBD_REPEAT_DELAY : KBD_REPEAT_RATE);
    // When not in numlock, and not shifted, remap num pad
    if (keycode >= KBD_HID_KEY_KEYPAD_1 &&
        keycode <= KBD_HID_KEY_KEYPAD_DECIMAL &&
        (!is_numlock || (key_shift && is_numlock)))
    {
        if (is_numlock)
            key_shift = false;
        switch (keycode)
        {
        case KBD_HID_KEY_KEYPAD_1:
            keycode = KBD_HID_KEY_END;
            break;
        case KBD_HID_KEY_KEYPAD_2:
            keycode = KBD_HID_KEY_ARROW_DOWN;
            break;
        case KBD_HID_KEY_KEYPAD_3:
            keycode = KBD_HID_KEY_PAGE_DOWN;
            break;
        case KBD_HID_KEY_KEYPAD_4:
            keycode = KBD_HID_KEY_ARROW_LEFT;
            break;
        case KBD_HID_KEY_KEYPAD_5:
            keycode = KBD_HID_KEY_NONE;
            break;
        case KBD_HID_KEY_KEYPAD_6:
            keycode = KBD_HID_KEY_ARROW_RIGHT;
            break;
        case KBD_HID_KEY_KEYPAD_7:
            keycode = KBD_HID_KEY_HOME;
            break;
        case KBD_HID_KEY_KEYPAD_8:
            keycode = KBD_HID_KEY_ARROW_UP;
            break;
        case KBD_HID_KEY_KEYPAD_9:
            keycode = KBD_HID_KEY_PAGE_UP;
            break;
        case KBD_HID_KEY_KEYPAD_0:
            keycode = KBD_HID_KEY_INSERT;
            break;
        case KBD_HID_KEY_KEYPAD_DECIMAL:
            keycode = KBD_HID_KEY_DELETE;
            break;
        }
    }
    // Find plain typed or AltGr character
    char ch = 0;
    if (keycode < 128 && !((modifier & (KBD_MODIFIER_LEFTALT |
                                        KBD_MODIFIER_LEFTGUI |
                                        KBD_MODIFIER_RIGHTGUI))))
    {
        bool use_shift = (key_shift && !is_capslock) ||
                         (key_shift && keycode > KBD_HID_KEY_Z) ||
                         (!key_shift && is_capslock && keycode <= KBD_HID_KEY_Z);
        if (modifier & KBD_MODIFIER_RIGHTALT)
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
            else if (keycode == KBD_HID_KEY_BACKSPACE)
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
        else if (keycode == KBD_HID_KEY_BACKSPACE)
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
    {
        switch (keycode)
        {
        case KBD_HID_KEY_DELETE:
            if (key_ctrl && key_alt)
            {
                kbd_key_queue_tail = kbd_key_queue_head;
                main_break();
                return;
            }
            break;
        case KBD_HID_KEY_NUM_LOCK:
            kdb_hid_leds ^= KBD_LED_NUMLOCK;
            kbd_send_leds();
            break;
        case KBD_HID_KEY_CAPS_LOCK:
            kdb_hid_leds ^= KBD_LED_CAPSLOCK;
            kbd_send_leds();
            break;
        case KBD_HID_KEY_SCROLL_LOCK:
            kdb_hid_leds ^= KBD_LED_SCROLLLOCK;
            kbd_send_leds();
            break;
        }
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
    case KBD_HID_KEY_ARROW_UP:
        return kbd_queue_seq("\33[A", "\33[1;%dA", ansi_modifier);
    case KBD_HID_KEY_ARROW_DOWN:
        return kbd_queue_seq("\33[B", "\33[1;%dB", ansi_modifier);
    case KBD_HID_KEY_ARROW_RIGHT:
        return kbd_queue_seq("\33[C", "\33[1;%dC", ansi_modifier);
    case KBD_HID_KEY_ARROW_LEFT:
        return kbd_queue_seq("\33[D", "\33[1;%dD", ansi_modifier);
    case KBD_HID_KEY_F1:
        return kbd_queue_seq("\33OP", "\33[1;%dP", ansi_modifier);
    case KBD_HID_KEY_F2:
        return kbd_queue_seq("\33OQ", "\33[1;%dQ", ansi_modifier);
    case KBD_HID_KEY_F3:
        return kbd_queue_seq("\33OR", "\33[1;%dR", ansi_modifier);
    case KBD_HID_KEY_F4:
        return kbd_queue_seq("\33OS", "\33[1;%dS", ansi_modifier);
    case KBD_HID_KEY_F5:
        return kbd_queue_seq_vt(15, ansi_modifier);
    case KBD_HID_KEY_F6:
        return kbd_queue_seq_vt(17, ansi_modifier);
    case KBD_HID_KEY_F7:
        return kbd_queue_seq_vt(18, ansi_modifier);
    case KBD_HID_KEY_F8:
        return kbd_queue_seq_vt(19, ansi_modifier);
    case KBD_HID_KEY_F9:
        return kbd_queue_seq_vt(10, ansi_modifier);
    case KBD_HID_KEY_F10:
        return kbd_queue_seq_vt(21, ansi_modifier);
    case KBD_HID_KEY_F11:
        return kbd_queue_seq_vt(23, ansi_modifier);
    case KBD_HID_KEY_F12:
        return kbd_queue_seq_vt(24, ansi_modifier);
    case KBD_HID_KEY_HOME:
        return kbd_queue_seq("\33[H", "\33[1;%dH", ansi_modifier);
    case KBD_HID_KEY_INSERT:
        return kbd_queue_seq_vt(2, ansi_modifier);
    case KBD_HID_KEY_DELETE:
        return kbd_queue_seq_vt(3, ansi_modifier);
    case KBD_HID_KEY_END:
        return kbd_queue_seq("\33[F", "\33[1;%dF", ansi_modifier);
    case KBD_HID_KEY_PAGE_UP:
        return kbd_queue_seq_vt(5, ansi_modifier);
    case KBD_HID_KEY_PAGE_DOWN:
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

void kbd_init(void)
{
    kbd_stop();
    kdb_hid_leds = KBD_LED_NUMLOCK;
    kbd_send_leds();
}

void kbd_task(void)
{
    if (kbd_repeat_keycode && absolute_time_diff_us(get_absolute_time(), kbd_repeat_timer) < 0)
    {
        if (KBD_KEY_BIT_VAL(kbd_keys, kbd_repeat_keycode) &&
            KBD_MODIFIER(kbd_keys) == kbd_repeat_modifier)
        {
            kbd_queue_key(KBD_MODIFIER(kbd_keys), kbd_repeat_keycode, false);
        }
        else
        {
            kbd_repeat_keycode = 0;
        }
    }
}

void kbd_stop(void)
{
    kbd_xram = 0xFFFF;
}

bool kbd_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - sizeof(kbd_keys))
        return false;
    kbd_xram = word;
    if (kbd_xram != 0xFFFF)
        memcpy(&xram[kbd_xram], kbd_keys, sizeof(kbd_keys));
    return true;
}

bool __in_flash("kbd_mount") kbd_mount(int slot, uint8_t const *desc_data, uint16_t desc_len)
{
    int conn_num = -1;
    for (int i = 0; i < KBD_MAX_KEYBOARDS; i++)
        if (!kbd_connections[i].valid)
            conn_num = i;
    if (conn_num < 0)
        return false;

    // Begion processing raw HID descriptor into kbd_connection_t
    kbd_connection_t *conn = &kbd_connections[conn_num];
    memset(conn, 0, sizeof(kbd_connection_t));
    for (int i = 0; i < 256; i++)
        conn->keycodes[i] = 0xFFFF;
    conn->slot = slot;

    // Use BTstack HID parser to parse the descriptor
    btstack_hid_usage_iterator_t iterator;
    btstack_hid_usage_iterator_init(&iterator, desc_data, desc_len, HID_REPORT_TYPE_INPUT);
    while (btstack_hid_usage_iterator_has_more(&iterator))
    {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&iterator, &item);
        if (item.usage_page == 0x07 && item.usage <= 0xFF) // Keyboards with valid keycodes
        {
            conn->valid = true;
            // Store report ID if this is the first one we encounter
            if (conn->report_id == 0 && item.report_id != 0xFFFF)
                conn->report_id = item.report_id;
            // 8 bits contain a keycode
            if (item.size == 8)
            {
                if (conn->codes_count == 0)
                {
                    conn->codes_offset = item.bit_pos;
                    conn->codes_count = 1;
                    DBG("Found keycode array start: offset=%d\n", conn->codes_offset);
                }
                else if (item.bit_pos == conn->codes_offset + (conn->codes_count * 8))
                {
                    conn->codes_count++;
                    DBG("Extending keycode array: count=%d\n", conn->codes_count);
                }
            }
            // 1 bit represents a keycode
            if (item.size == 1)
            {
                conn->keycodes[item.usage] = item.bit_pos;
                DBG("Found individual keycode bit: usage=0x%02x, offset=%d\n", item.usage, item.bit_pos);
            }
        }
    }
    return conn->valid;
}

// Clean up descriptor when device is disconnected.
bool kbd_umount(int slot)
{
    kbd_connection_t *conn = kbd_get_connection_by_slot(slot);
    if (conn == NULL)
        return false;
    conn->valid = false;
    return true;
}

void kbd_report(int slot, uint8_t const *data, size_t size)
{
    kbd_connection_t *conn = kbd_get_connection_by_slot(slot);
    if (conn == NULL)
        return;

    const uint8_t *report_data = data;
    uint16_t report_data_len = size;

    // Handle report ID if present
    if (conn->report_id != 0)
    {
        if (report_data_len == 0 || report_data[0] != conn->report_id)
            return;
        // Skip report ID byte
        report_data++;
        report_data_len--;
    }

    // Swap in a new keys bit array
    uint32_t old_keys[8];
    memcpy(&old_keys, conn->keys, sizeof(conn->keys));
    memset(conn->keys, 0, sizeof(conn->keys));

    // Extract from keycode array
    for (int i = 0; i < conn->codes_count; i++)
    {
        uint16_t bit_offset = conn->codes_offset + (i * 8);
        uint8_t keycode = (uint8_t)des_extract_bits(report_data, report_data_len,
                                                    bit_offset, 8);
        if (keycode == 1)
        {
            // ignore reports when in phantom/overflow condition
            memcpy(conn->keys, &old_keys, sizeof(conn->keys));
            return;
        }
        KBD_KEY_BIT_SET(conn->keys, keycode);
    }

    // Extract individual keycode bits
    for (int i = 0; i <= 0xFF; i++)
    {
        uint32_t bit_val = des_extract_bits(report_data, report_data_len,
                                            conn->keycodes[i], 1);
        if (bit_val)
            KBD_KEY_BIT_SET(conn->keys, i);
    }

    // Merge all keyboards into one report so we have
    // an updated KBD_MODIFIER(kbd_keys).
    memset(kbd_keys, 0, sizeof(kbd_keys));
    for (int k = 0; k < 8; k++)
        for (int i = 0; i < KBD_MAX_KEYBOARDS; i++)
            kbd_keys[k] |= kbd_connections[i].keys[k];

    // Find new key down events after new kbd_keys is made
    // so we have the latest modifiers.
    for (int i = 0; i < 128; i++)
    {
        bool curr = KBD_KEY_BIT_VAL(conn->keys, i);
        bool prev = KBD_KEY_BIT_VAL(old_keys, i);
        if (curr && !prev)
            kbd_queue_key(KBD_MODIFIER(kbd_keys), i, true);
    }

    // Check for no keys pressed
    bool any_key = false;
    kbd_keys[0] &= ~0xF;
    for (int k = 0; k < 8; k++)
        if (kbd_keys[k])
            any_key = true;
    if (!any_key)
        kbd_keys[0] |= 1;

    // NUMLOCK CAPSLOCK SCROLLLOCK
    kbd_keys[0] |= (kdb_hid_leds & 7) << 1;

    // Send it to xram
    if (kbd_xram != 0xFFFF)
        memcpy(&xram[kbd_xram], kbd_keys, sizeof(kbd_keys));
}
