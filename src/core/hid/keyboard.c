/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/xram.h"
#include "core/hid/keyboard.h"
#include "core/hid/hid.h"
#include "core/hid/keymap.h"
#include "machine.h"
#include "core/hid/usage.h"
#include <stdio.h>
#include <string.h>

// RP6502 and Windows boots like an IBM AT with num lock on.
// The Raspberry Pi Keyboard uses num lock to enable a num pad over
// letter keys; it was designed for Linux where num lock boots off.
static const struct
{
    uint16_t vid;
    uint16_t pid;
} keyboard_numlock_off_at_boot[] = {
    {0x04D9, 0x0006}, // Raspberry Pi Keyboard
};

static uint16_t keyboard_xram;
static uint8_t keyboard_hid_leds;
static uint32_t keyboard_keys[8];
static keyboard_connection_t keyboard_connections[KEYBOARD_MAX_KEYBOARDS];

#define KEYBOARD_KEY_BIT_SET(data, keycode) ((data)[(keycode) >> 5] |= 1 << ((keycode) & 31))
#define KEYBOARD_KEY_BIT_VAL(data, keycode) ((data)[(keycode) >> 5] & (1 << ((keycode) & 31)))

// Direct access to the modifier byte of keyboard_keys
#define KEYBOARD_MODIFIER(keys) ((uint8_t *)keys)[HID_KEY_CONTROL_LEFT >> 3]

static keyboard_connection_t *keyboard_get_connection_by_slot(int slot)
{
    for (int i = 0; i < KEYBOARD_MAX_KEYBOARDS; i++)
        if (keyboard_connections[i].valid && keyboard_connections[i].slot == slot)
            return &keyboard_connections[i];
    return NULL;
}

uint8_t keyboard_get_report_id(int slot)
{
    keyboard_connection_t *conn = keyboard_get_connection_by_slot(slot);
    return conn ? conn->report_id : 0;
}

static void keyboard_merge_keys(void)
{
    memset(keyboard_keys, 0, sizeof(keyboard_keys));
    for (int k = 0; k < 8; k++)
        for (int i = 0; i < KEYBOARD_MAX_KEYBOARDS; i++)
            keyboard_keys[k] |= keyboard_connections[i].keys[k];
}

/* Word 0's low bits are not keys: bit 0 says nothing is pressed and bits
 * 1-3 are the lock LEDs, so they are restated every time the block goes
 * out. */
static void keyboard_publish(void)
{
    bool any_key = false;
    keyboard_keys[0] &= ~0xF;
    for (int k = 0; k < 8; k++)
        if (keyboard_keys[k])
            any_key = true;
    if (!any_key)
        keyboard_keys[0] |= 1;
    keyboard_keys[0] |= (keyboard_hid_leds & 7) << 1; // NUMLOCK CAPSLOCK SCROLLLOCK
    if (keyboard_xram != 0xFFFF)
        memcpy((uint8_t *)&xram[keyboard_xram], keyboard_keys, sizeof(keyboard_keys));
}

static void keyboard_send_leds()
{
    hid_set_leds(keyboard_hid_leds);
}

uint8_t keyboard_keypad_nav(uint8_t hid_usage)
{
    switch (hid_usage)
    {
    case HID_KEY_KEYPAD_1: return HID_KEY_END;
    case HID_KEY_KEYPAD_2: return HID_KEY_ARROW_DOWN;
    case HID_KEY_KEYPAD_3: return HID_KEY_PAGE_DOWN;
    case HID_KEY_KEYPAD_4: return HID_KEY_ARROW_LEFT;
    case HID_KEY_KEYPAD_6: return HID_KEY_ARROW_RIGHT;
    case HID_KEY_KEYPAD_7: return HID_KEY_HOME;
    case HID_KEY_KEYPAD_8: return HID_KEY_ARROW_UP;
    case HID_KEY_KEYPAD_9: return HID_KEY_PAGE_UP;
    case HID_KEY_KEYPAD_0: return HID_KEY_INSERT;
    case HID_KEY_KEYPAD_DECIMAL: return HID_KEY_DELETE;
    }
    return HID_KEY_NONE; /* KP5, and anything not on the keypad */
}

int keyboard_vt_mod(bool shift, bool alt, bool ctrl, bool gui)
{
    return 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0) + (gui ? 8 : 0);
}

/* ESC[1;{mod}{c1} when modified, else the bare ESC{c0}{c1} -- ESC[A for an
 * arrow, ESC O P for F1. */
static size_t keyboard_vt100(char *out, size_t cap, char c0, char c1, int ansi_mod)
{
    if (ansi_mod == 1)
        return (size_t)snprintf(out, cap, "\33%c%c", c0, c1);
    return (size_t)snprintf(out, cap, "\33[1;%d%c", ansi_mod, c1);
}

// The numbered form: ESC[{num}~, or ESC[{num};{mod}~ when modified.
static size_t keyboard_vt220(char *out, size_t cap, int num, int ansi_mod)
{
    if (ansi_mod == 1)
        return (size_t)snprintf(out, cap, "\33[%d~", num);
    return (size_t)snprintf(out, cap, "\33[%d;%d~", num, ansi_mod);
}

/* Two contiguous HID runs, so the table is an index rather than a search.
 * intro 'O' or '[' is the VT100 form and code is its final character; intro 0
 * is the VT220 form and code is its number. */
static const struct
{
    char intro;
    uint8_t code;
} keyboard_vt_keys[] = {
    /* 0x3A..0x45, F1 to F12 */
    {'O', 'P'}, {'O', 'Q'}, {'O', 'R'}, {'O', 'S'},
    {0, 15}, {0, 17}, {0, 18}, {0, 19},
    {0, 20}, {0, 21}, {0, 23}, {0, 24},
    /* 0x46..0x48, PrintScreen, ScrollLock and Pause send nothing */
    {0, 0}, {0, 0}, {0, 0},
    /* 0x49..0x52, Insert to Up */
    {0, 2}, {'[', 'H'}, {0, 5}, {0, 3}, {'[', 'F'},
    {0, 6}, {'[', 'C'}, {'[', 'D'}, {'[', 'B'}, {'[', 'A'}};

size_t keyboard_vt_seq(char *out, size_t cap, uint8_t hid_usage, int ansi_mod)
{
    if (hid_usage < 0x3A || hid_usage > 0x52)
        return 0;
    const char intro = keyboard_vt_keys[hid_usage - 0x3A].intro;
    const uint8_t code = keyboard_vt_keys[hid_usage - 0x3A].code;
    if (intro)
        return keyboard_vt100(out, cap, intro, (char)code, ansi_mod);
    if (code)
        return keyboard_vt220(out, cap, code, ansi_mod);
    return 0;
}

char keyboard_ctrl_promote(char ch, uint8_t keycode)
{
    if (ch >= '`' && ch <= '~')
        return (char)(ch - 96);
    if (ch >= '@' && ch <= '_')
        return (char)(ch - 64);
    if (keycode == HID_KEY_BACKSPACE)
        return '\b';
    /* Enter, Tab and Escape are C0 controls already, so Ctrl has nothing left
     * to promote and the key still types itself. */
    if ((unsigned char)ch < 0x20)
        return ch;
    return 0;
}

void HOST_IN_FLASH("keyboard_init") keyboard_init(void)
{
    keyboard_stop();
    keyboard_hid_leds = KEYBOARD_LED_NUMLOCK;
    keyboard_send_leds();
}

void keyboard_stop(void)
{
    keyboard_xram = 0xFFFF;
}

bool HOST_IN_FLASH("keyboard_mount") keyboard_mount(int slot, const keyboard_connection_t *desc,
                                       uint16_t vendor_id, uint16_t product_id)
{
    if (!desc->valid)
        return false;
    for (int i = 0; i < KEYBOARD_MAX_KEYBOARDS; i++)
    {
        if (keyboard_connections[i].valid)
            continue;
        keyboard_connections[i] = *desc;
        keyboard_connections[i].slot = slot;

        if (hid_boot_enumerating())
            for (size_t k = 0; k < sizeof(keyboard_numlock_off_at_boot) / sizeof(keyboard_numlock_off_at_boot[0]); k++)
                if (keyboard_numlock_off_at_boot[k].vid == vendor_id &&
                    keyboard_numlock_off_at_boot[k].pid == product_id)
                {
                    keyboard_hid_leds &= ~KEYBOARD_LED_NUMLOCK;
                    keyboard_send_leds();
                    break;
                }
        return true;
    }
    return false;
}

// Clean up descriptor when device is disconnected.
bool keyboard_umount(int slot)
{
    keyboard_connection_t *conn = keyboard_get_connection_by_slot(slot);
    if (conn == NULL)
        return false;
    conn->valid = false;
    memset(conn->keys, 0, sizeof(conn->keys));
    keyboard_merge_keys();
    return true;
}

void keyboard_report(int slot, uint8_t const *data, size_t size)
{
    keyboard_connection_t *conn = keyboard_get_connection_by_slot(slot);
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
    memcpy(old_keys, conn->keys, sizeof(conn->keys));
    memset(conn->keys, 0, sizeof(conn->keys));

    // Extract from keycode array
    for (int i = 0; i < conn->codes_count; i++)
    {
        uint16_t bit_offset = conn->codes_offset + (i * 8);
        uint8_t keycode = (uint8_t)hid_extract_bits(report_data, report_data_len,
                                                    bit_offset, 8);
        if (keycode == 1)
        {
            // ignore reports when in phantom/overflow condition
            memcpy(conn->keys, old_keys, sizeof(conn->keys));
            return;
        }
        KEYBOARD_KEY_BIT_SET(conn->keys, keycode);
    }

    // Extract individual keycode bits
    for (int r = 0; r < KEYBOARD_KEY_RUNS; r++)
    {
        const keyboard_key_run_t *run = &conn->runs[r];
        if (!run->count)
            break;
        for (uint16_t i = 0; i < run->count; i++)
            if (hid_extract_bits(report_data, report_data_len, run->bit_pos + i, 1))
                KEYBOARD_KEY_BIT_SET(conn->keys, run->usage_min + i);
    }

    // Merge all keyboards into one report so we have
    // an updated KEYBOARD_MODIFIER(keyboard_keys).
    keyboard_merge_keys();

    // Find new key down events after new keyboard_keys is made
    // so we have the latest modifiers.
    for (int i = 0; i < 128; i++)
    {
        bool curr = KEYBOARD_KEY_BIT_VAL(conn->keys, i);
        bool prev = KEYBOARD_KEY_BIT_VAL(old_keys, i);
        if (curr && !prev)
            keymap_on_key(KEYBOARD_MODIFIER(keyboard_keys), i);
    }

    // Check for releasing ALT key during ALT mode.
    keymap_on_modifiers(KEYBOARD_MODIFIER(keyboard_keys));

    keyboard_publish();
}

bool keyboard_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - sizeof(keyboard_keys))
        return false;
    keyboard_xram = word;
    keyboard_publish();
    return true;
}

uint8_t keyboard_get_modifier(void)
{
    return KEYBOARD_MODIFIER(keyboard_keys);
}

bool keyboard_key_down(uint8_t keycode)
{
    return KEYBOARD_KEY_BIT_VAL(keyboard_keys, keycode) != 0;
}

uint8_t keyboard_get_leds(void)
{
    return keyboard_hid_leds;
}

void keyboard_toggle_lock(uint8_t bit)
{
    keyboard_hid_leds ^= bit;
    keyboard_send_leds();
    keyboard_publish();
}

/* A host whose OS decodes its own keyboard sets the bits a report would
 * have set. Keycodes 0-3 are reserved -- none, and the rollover errors --
 * and their bits in word 0 carry the no-keys and lock flags, so a key
 * never touches them. */
void keyboard_hid_set(uint8_t keycode, bool down)
{
    if (keycode < 4)
        return;
    if (down)
        KEYBOARD_KEY_BIT_SET(keyboard_keys, keycode);
    else
        keyboard_keys[keycode >> 5] &= ~(1u << (keycode & 31));
    keyboard_publish();
}

