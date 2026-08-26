/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/main.h"
#include "core/hid/keyboard.h"
#include "core/hid/hid.h"
#include "host.h"
#include "core/hid/usage.h"

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
            keyboard_spell_key(KEYBOARD_MODIFIER(keyboard_keys), i);
    }

    // Check for releasing ALT key during ALT mode.
    keyboard_spell_modifiers(KEYBOARD_MODIFIER(keyboard_keys));

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

