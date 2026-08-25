/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/main.h"
#include "core/api/oem.h"
#include "core/api/uni.h"
#include "core/hid/kbd.h"
#include "core/hid/kbt.h"
#include "core/hid/hid.h"
#include "core/cfg.h"
#include "host.h"
#include "core/hid/usage.h"
#include <stdio.h>
/* The case-insensitive compares a layout name is matched with. Named by
 * POSIX rather than by C, and a host that has no such header supplies
 * one — see src/host/windows. */
#include <strings.h>

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_KBD)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// RP6502 and Windows boots like an IBM AT with num lock on.
// The Raspberry Pi Keyboard uses num lock to enable a num pad over
// letter keys; it was designed for Linux where num lock boots off.
static const struct
{
    uint16_t vid;
    uint16_t pid;
} kbd_numlock_off_at_boot[] = {
    {0x04D9, 0x0006}, // Raspberry Pi Keyboard
};

static uint16_t kbd_xram;
static uint8_t kbd_hid_leds;
static uint32_t kbd_keys[8];
static kbd_connection_t kbd_connections[KBD_MAX_KEYBOARDS];

#define KBD_KEY_BIT_SET(data, keycode) ((data)[(keycode) >> 5] |= 1 << ((keycode) & 31))
#define KBD_KEY_BIT_VAL(data, keycode) ((data)[(keycode) >> 5] & (1 << ((keycode) & 31)))

// Direct access to the modifier byte of kbd_keys
#define KBD_MODIFIER(keys) ((uint8_t *)keys)[HID_KEY_CONTROL_LEFT >> 3]

static kbd_connection_t *kbd_get_connection_by_slot(int slot)
{
    for (int i = 0; i < KBD_MAX_KEYBOARDS; i++)
        if (kbd_connections[i].valid && kbd_connections[i].slot == slot)
            return &kbd_connections[i];
    return NULL;
}

uint8_t kbd_get_report_id(int slot)
{
    kbd_connection_t *conn = kbd_get_connection_by_slot(slot);
    return conn ? conn->report_id : 0;
}

static void kbd_merge_keys(void)
{
    memset(kbd_keys, 0, sizeof(kbd_keys));
    for (int k = 0; k < 8; k++)
        for (int i = 0; i < KBD_MAX_KEYBOARDS; i++)
            kbd_keys[k] |= kbd_connections[i].keys[k];
}

/* Word 0's low bits are not keys: bit 0 says nothing is pressed and bits
 * 1-3 are the lock LEDs, so they are restated every time the block goes
 * out. */
static void kbd_publish(void)
{
    bool any_key = false;
    kbd_keys[0] &= ~0xF;
    for (int k = 0; k < 8; k++)
        if (kbd_keys[k])
            any_key = true;
    if (!any_key)
        kbd_keys[0] |= 1;
    kbd_keys[0] |= (kbd_hid_leds & 7) << 1; // NUMLOCK CAPSLOCK SCROLLLOCK
    if (kbd_xram != 0xFFFF)
        memcpy((uint8_t *)&xram[kbd_xram], kbd_keys, sizeof(kbd_keys));
}

static void kbd_send_leds()
{
    hid_set_leds(kbd_hid_leds);
}

void HOST_IN_FLASH("kbd_init") kbd_init(void)
{
    kbd_stop();
    kbd_hid_leds = KEYBOARD_LED_NUMLOCK;
    kbd_send_leds();
    kbt_init();
}

void kbd_stop(void)
{
    kbd_xram = 0xFFFF;
}

bool HOST_IN_FLASH("kbd_mount") kbd_mount(int slot, const kbd_connection_t *desc,
                                       uint16_t vendor_id, uint16_t product_id)
{
    if (!desc->valid)
        return false;
    for (int i = 0; i < KBD_MAX_KEYBOARDS; i++)
    {
        if (kbd_connections[i].valid)
            continue;
        kbd_connections[i] = *desc;
        kbd_connections[i].slot = slot;

        if (hid_boot_enumerating())
            for (size_t k = 0; k < sizeof(kbd_numlock_off_at_boot) / sizeof(kbd_numlock_off_at_boot[0]); k++)
                if (kbd_numlock_off_at_boot[k].vid == vendor_id &&
                    kbd_numlock_off_at_boot[k].pid == product_id)
                {
                    kbd_hid_leds &= ~KEYBOARD_LED_NUMLOCK;
                    kbd_send_leds();
                    break;
                }
        return true;
    }
    return false;
}

// Clean up descriptor when device is disconnected.
bool kbd_umount(int slot)
{
    kbd_connection_t *conn = kbd_get_connection_by_slot(slot);
    if (conn == NULL)
        return false;
    conn->valid = false;
    memset(conn->keys, 0, sizeof(conn->keys));
    kbd_merge_keys();
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
        KBD_KEY_BIT_SET(conn->keys, keycode);
    }

    // Extract individual keycode bits
    for (int r = 0; r < KBD_KEY_RUNS; r++)
    {
        const kbd_key_run_t *run = &conn->runs[r];
        if (!run->count)
            break;
        for (uint16_t i = 0; i < run->count; i++)
            if (hid_extract_bits(report_data, report_data_len, run->bit_pos + i, 1))
                KBD_KEY_BIT_SET(conn->keys, run->usage_min + i);
    }

    // Merge all keyboards into one report so we have
    // an updated KBD_MODIFIER(kbd_keys).
    kbd_merge_keys();

    // Find new key down events after new kbd_keys is made
    // so we have the latest modifiers.
    for (int i = 0; i < 128; i++)
    {
        bool curr = KBD_KEY_BIT_VAL(conn->keys, i);
        bool prev = KBD_KEY_BIT_VAL(old_keys, i);
        if (curr && !prev)
            kbt_key_down(KBD_MODIFIER(kbd_keys), i);
    }

    // Check for releasing ALT key during ALT mode.
    kbt_modifiers(KBD_MODIFIER(kbd_keys));

    kbd_publish();
}

bool kbd_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - sizeof(kbd_keys))
        return false;
    kbd_xram = word;
    kbd_publish();
    return true;
}

uint8_t kbd_get_modifier(void)
{
    return KBD_MODIFIER(kbd_keys);
}

bool kbd_key_down(uint8_t keycode)
{
    return KBD_KEY_BIT_VAL(kbd_keys, keycode) != 0;
}

uint8_t kbd_get_leds(void)
{
    return kbd_hid_leds;
}

void kbd_toggle_lock(uint8_t bit)
{
    kbd_hid_leds ^= bit;
    kbd_send_leds();
    kbd_publish();
}

/* A host whose OS decodes its own keyboard sets the bits a report would
 * have set. Keycodes 0-3 are reserved -- none, and the rollover errors --
 * and their bits in word 0 carry the no-keys and lock flags, so a key
 * never touches them. */
void kbd_hid_set(uint8_t keycode, bool down)
{
    if (keycode < 4)
        return;
    if (down)
        KBD_KEY_BIT_SET(kbd_keys, keycode);
    else
        kbd_keys[keycode >> 5] &= ~(1u << (keycode & 31));
    kbd_publish();
}

