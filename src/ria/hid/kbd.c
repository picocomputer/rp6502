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
#include "hid/des.h"
#include "usb/hid.h"
#include "fatfs/ff.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

#define DEBUG_RIA_HID_KBD

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_KBD)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define KBD_REPEAT_DELAY 500000
#define KBD_REPEAT_RATE 30000

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
    uint8_t slot;     // HID slot
    uint32_t keys[8]; // last report, bits 0-3 unused

    // HID descriptor parsing fields
    uint8_t report_id;              // If non zero, the first report byte must match and will be skipped
    uint16_t modifier_offsets[8];   // Bit offsets for each of the 8 modifier keys (0xFFFF = not present)
    uint16_t keycode_offset;        // Offset in bits for keycode array
    uint8_t keycode_count;          // Number of keycodes in array
    bool has_keycode_bitmap;        // 256-bit bitmap where each bit represents a keycode
    uint16_t keycode_bitmap_offset; // Offset in bits for the bitmap
    uint16_t keycode_bitmap_size;   // Size in bits for the bitmap (usually 256)
    uint16_t keycode_offsets[32];   // Individual keycode bit positions (for sparse keyboards)
    uint8_t keycode_bit_count;      // Number of individual keycode bits found
} kbd_connection_t;

#define KBD_MAX_KEYBOARDS 4
static kbd_connection_t kbd_connections[KBD_MAX_KEYBOARDS];

#define KBD_MODIFIER(keys) ((uint8_t *)keys)[HID_KEY_CONTROL_LEFT >> 3]

#define KBD_KEY_QUEUE(pos) kbd_key_queue[(pos) & 0x0F]

#define HID_KEYCODE_TO_UNICODE_(kb) HID_KEYCODE_TO_UNICODE_##kb
#define HID_KEYCODE_TO_UNICODE(kb) HID_KEYCODE_TO_UNICODE_(kb)
static DWORD const __in_flash("keycode_to_unicode")
    KEYCODE_TO_UNICODE[128][4] = {HID_KEYCODE_TO_UNICODE(RP6502_KEYBOARD)};

// Parse HID descriptor to extract keyboard report structure
static void kbd_parse_descriptor(kbd_connection_t *conn, uint8_t const *desc_data, uint16_t desc_len)
{
    // Initialize all fields
    memset(conn, 0, sizeof(kbd_connection_t));
    for (int i = 0; i < 8; i++)
        conn->modifier_offsets[i] = 0xFFFF;
    for (int i = 0; i < 32; i++)
        conn->keycode_offsets[i] = 0xFFFF;

    if (desc_len == 0)
        return;

    // Use BTstack HID parser to parse the descriptor
    btstack_hid_usage_iterator_t iterator;
    btstack_hid_usage_iterator_init(&iterator, desc_data, desc_len, HID_REPORT_TYPE_INPUT);

    bool found_keyboard_input = false;

    while (btstack_hid_usage_iterator_has_more(&iterator))
    {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&iterator, &item);

        DBG("HID item: page=0x%02x, usage=0x%02x, report_id=0x%04x, size=%d, bit_pos=%d\n",
            item.usage_page, item.usage, item.report_id, item.size, item.bit_pos);

        bool get_report_id = false;

        if (item.usage_page == 0x07) // Keyboard/Keypad page
        {
            get_report_id = true;
            found_keyboard_input = true;

            if (item.usage >= 0xE0 && item.usage <= 0xE7) // Modifier keys
            {
                uint8_t mod_index = item.usage - 0xE0; // Convert to 0-7 index
                if (mod_index < 8 && conn->modifier_offsets[mod_index] == 0xFFFF)
                {
                    conn->modifier_offsets[mod_index] = item.bit_pos;
                    DBG("Found modifier key 0x%02x: bit_pos=%d\n", item.usage, item.bit_pos);
                }
            }
            else if (item.usage <= 0xFF)
            {
                if (item.size == 8 && conn->keycode_count == 0)
                {
                    conn->keycode_offset = item.bit_pos;
                    conn->keycode_count = 1;
                    DBG("Found keycode array start: offset=%d\n", conn->keycode_offset);
                }
                else if (item.size == 8 &&
                         item.bit_pos == conn->keycode_offset + (conn->keycode_count * 8))
                {
                    // This is a continuation of the keycode array
                    conn->keycode_count++;
                    DBG("Extending keycode array: count=%d\n", conn->keycode_count);
                }
                else if (item.size == 1)
                {
                    // Check if this looks like the start of a contiguous bitmap
                    // Many keyboards have bitmaps starting from usage 0x00 or near modifiers
                    if (!conn->has_keycode_bitmap && conn->keycode_bit_count == 0)
                    {
                        // This could be the start of a bitmap - record it and see if more follow
                        conn->has_keycode_bitmap = true;
                        conn->keycode_bitmap_offset = item.bit_pos;
                        conn->keycode_bitmap_size = 1;
                        DBG("Found potential keycode bitmap start: offset=%d, usage=0x%02x\n", item.bit_pos, item.usage);
                    }
                    else if (conn->has_keycode_bitmap &&
                             item.bit_pos == conn->keycode_bitmap_offset + conn->keycode_bitmap_size)
                    {
                        // This continues the bitmap - extend it
                        conn->keycode_bitmap_size++;
                        DBG("Extended keycode bitmap: size=%d, usage=0x%02x\n", conn->keycode_bitmap_size, item.usage);

                        // If we've seen enough contiguous bits, this is likely a bitmap
                        if (conn->keycode_bitmap_size >= 64) // Reasonable threshold for bitmap detection
                        {
                            DBG("Confirmed keycode bitmap with %d bits\n", conn->keycode_bitmap_size);
                        }
                    }
                    else if (!conn->has_keycode_bitmap)
                    {
                        // Individual sparse keycode bits - fall back if bitmap detection fails
                        if (conn->keycode_bit_count < 32)
                        {
                            conn->keycode_offsets[conn->keycode_bit_count] = item.bit_pos;
                            conn->keycode_bit_count++;
                            DBG("Found individual keycode bit: usage=0x%02x, offset=%d\n", item.usage, item.bit_pos);
                        }
                    }
                }
            }
        }

        // Store report ID if this is the first one we encounter
        if (get_report_id && conn->report_id == 0 && item.report_id != 0xFFFF)
            conn->report_id = item.report_id;
    }

    // Post-process bitmap detection: if we have a very small "bitmap",
    // it's probably just individual keys, not a true bitmap
    if (conn->has_keycode_bitmap && conn->keycode_bitmap_size < 32)
    {
        DBG("Converting small bitmap (%d bits) to individual keycode bits\n", conn->keycode_bitmap_size);
        conn->has_keycode_bitmap = false;
        conn->keycode_bit_count = 0; // We'll need to re-parse for proper usage mapping
        // Note: This is a simplified fallback - ideally we'd store usage info during parsing
    }

    conn->valid = found_keyboard_input && (conn->modifier_offsets[0] != 0xFFFF || conn->keycode_count ||
                                           conn->has_keycode_bitmap || conn->keycode_bit_count > 0);

    // Debug print parsed descriptor
    DBG("kbd_parse_descriptor: report_id=%d, valid=%d\n", conn->report_id, conn->valid);
    for (int i = 0; i < 8; i++)
        if (conn->modifier_offsets[i] != 0xFFFF)
            DBG("    Modifier %d: offset=%d\n", i, conn->modifier_offsets[i]);
    DBG("  Keycode array: offset=%d, count=%d\n",
        conn->keycode_offset, conn->keycode_count);
    DBG("  Keycode bitmap: has=%d, offset=%d, size=%d\n",
        conn->has_keycode_bitmap, conn->keycode_bitmap_offset, conn->keycode_bitmap_size);
    DBG("  Individual bits: count=%d\n", conn->keycode_bit_count);
}

static kbd_connection_t *kbd_get_connection_by_slot(uint8_t slot)
{
    for (int i = 0; i < KBD_MAX_KEYBOARDS; i++)
        if (kbd_connections[i].valid && kbd_connections[i].slot == slot)
            return &kbd_connections[i];
    return NULL;
}

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
    kbd_repeat_modifier = modifier;
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
        if (kbd_keys[kbd_repeat_keycode >> 5] & (1 << (kbd_repeat_keycode & 31)) &&
            KBD_MODIFIER(kbd_keys) == kbd_repeat_modifier)
        {
            kbd_queue_key(KBD_MODIFIER(kbd_keys), kbd_repeat_keycode, false);
            return;
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
    if (word != 0xFFFF && word > 0x10000 - sizeof(kbd_keys))
        return false;
    kbd_xram = word;
    if (kbd_xram != 0xFFFF)
        memcpy(&xram[kbd_xram], kbd_keys, sizeof(kbd_keys));
    return true;
}

// Parse HID report descriptor
bool kbd_mount(uint8_t slot, uint8_t const *desc_data, uint16_t desc_len)
{
    int conn_num = -1;
    for (int i = 0; i < KBD_MAX_KEYBOARDS; i++)
        if (!kbd_connections[i].valid)
            conn_num = i;
    if (conn_num < 0)
        return false;

    kbd_connection_t *conn = &kbd_connections[conn_num];

    // Process raw HID descriptor into conn
    kbd_parse_descriptor(conn, desc_data, desc_len);
    conn->slot = slot;

    DBG("kbd_mount: slot=%d, valid=%d, keycode_count=%d, has_bitmap=%d, keycode_bits=%d\n",
        slot, conn->valid, conn->keycode_count,
        conn->has_keycode_bitmap, conn->keycode_bit_count);

    return conn->valid;
}

// Clean up descriptor when device is disconnected.
void kbd_umount(uint8_t slot)
{
    kbd_connection_t *conn = kbd_get_connection_by_slot(slot);
    if (conn == NULL)
        return;
    conn->valid = false;
}

void kbd_report(uint8_t slot, uint8_t const *data, size_t size)
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

    // Extract modifier bits
    for (int i = 0; i < 8; i++)
    {
        if (conn->modifier_offsets[i] != 0xFFFF)
        {
            uint32_t bit_val = des_extract_bits(report_data, report_data_len,
                                                conn->modifier_offsets[i], 1);
            if (bit_val)
                KBD_MODIFIER(conn->keys) |= (1 << i);
        }
    }

    // Extract keycode array
    for (int i = 0; i < conn->keycode_count; i++)
    {
        uint16_t bit_offset = conn->keycode_offset + (i * 8);
        uint8_t keycode = (uint8_t)des_extract_bits(report_data, report_data_len,
                                                    bit_offset, 8);
        if (keycode == 1)
        {
            // ignore reports when in phantom/overflow condition
            memcpy(conn->keys, &old_keys, sizeof(conn->keys));
            return;
        }
        conn->keys[keycode >> 5] |= 1 << (keycode & 31);
    }

    // Extract keycode bitmap if present (256-bit bitmap where each bit = keycode)
    if (conn->has_keycode_bitmap)
    {
        // Extract the bitmap in chunks and set corresponding key bits
        uint16_t bitmap_bytes = (conn->keycode_bitmap_size + 7) / 8; // Round up to bytes
        for (uint16_t byte_idx = 0; byte_idx < bitmap_bytes && byte_idx < 32; byte_idx++)
        {
            uint8_t bitmap_byte = (uint8_t)des_extract_bits(report_data, report_data_len,
                                                            conn->keycode_bitmap_offset + (byte_idx * 8), 8);

            // Each bit in this byte represents a keycode
            for (int bit_idx = 0; bit_idx < 8; bit_idx++)
            {
                if (bitmap_byte & (1 << bit_idx))
                {
                    uint8_t keycode = (byte_idx * 8) + bit_idx;
                    if (keycode < 128) // Only handle standard keycodes
                    {
                        conn->keys[keycode >> 5] |= 1 << (keycode & 31);
                    }
                }
            }
        }
    }

    // Extract individual keycode bits if present (sparse keyboards)
    for (int i = 0; i < conn->keycode_bit_count; i++)
    {
        if (conn->keycode_offsets[i] != 0xFFFF)
        {
            uint32_t bit_val = des_extract_bits(report_data, report_data_len,
                                                conn->keycode_offsets[i], 1);
            if (bit_val)
            {
                // For individual bits, we need to figure out which keycode this represents
                // This is more complex and depends on the specific descriptor layout
                // For now, we'll store the bit position as the keycode
                uint8_t keycode = i + 32; // Offset to avoid conflicts with array keycodes
                conn->keys[keycode >> 5] |= 1 << (keycode & 31);
            }
        }
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
        bool curr = conn->keys[i >> 5] & (1 << (i & 31));
        bool prev = old_keys[i >> 5] & (1 << (i & 31));
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
