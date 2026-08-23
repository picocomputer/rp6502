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
#include <pico.h>
#include <stdio.h>
/* The case-insensitive compares a layout name is matched with. Named by
 * POSIX rather than by C, and a host that has no such header supplies
 * one — see src/host/win. */
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

static void kbd_send_leds()
{
    hid_set_leds(kbd_hid_leds);
}

void __in_flash("kbd_init") kbd_init(void)
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

static bool __in_flash("kbd_parse") kbd_parse_field(const hid_field_t *field, void *context)
{
    kbd_connection_t *conn = (kbd_connection_t *)context;
    if (field->usage_page == 0x07 && field->usage <= 0xFF)
    {
        conn->valid = true;
        if (conn->report_id == 0 && field->report_id != 0xFFFF)
            conn->report_id = field->report_id;
        if (field->size == 8)
        {
            if (conn->codes_count == 0)
            {
                conn->codes_offset = field->bit_pos;
                conn->codes_count = 1;
            }
            else if (field->bit_pos == conn->codes_offset + (conn->codes_count * 8))
            {
                conn->codes_count++;
            }
        }
        if (field->size == 1)
            conn->keycodes[field->usage] = field->bit_pos;
    }
    return true;
}

bool __in_flash("kbd_mount") kbd_mount(int slot, uint8_t const *desc_data, uint16_t desc_len,
                                       uint16_t vendor_id, uint16_t product_id)
{
    int conn_num = -1;
    for (int i = 0; i < KBD_MAX_KEYBOARDS; i++)
        if (!kbd_connections[i].valid)
        {
            conn_num = i;
            break;
        }
    if (conn_num < 0)
        return false;

    // Begin processing raw HID descriptor into kbd_connection_t
    kbd_connection_t *conn = &kbd_connections[conn_num];
    memset(conn, 0, sizeof(kbd_connection_t));
    for (int i = 0; i < 256; i++)
        conn->keycodes[i] = 0xFFFF;
    conn->slot = slot;

    hid_descriptor_parse(desc_data, desc_len, kbd_parse_field, conn);
    if (conn->valid && hid_boot_enumerating())
        for (size_t i = 0; i < sizeof(kbd_numlock_off_at_boot) / sizeof(kbd_numlock_off_at_boot[0]); i++)
            if (kbd_numlock_off_at_boot[i].vid == vendor_id &&
                kbd_numlock_off_at_boot[i].pid == product_id)
            {
                kbd_hid_leds &= ~KEYBOARD_LED_NUMLOCK;
                kbd_send_leds();
                break;
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
    for (int i = 0; i <= 0xFF; i++)
    {
        if (conn->keycodes[i] == 0xFFFF)
            continue;
        uint32_t bit_val = hid_extract_bits(report_data, report_data_len,
                                            conn->keycodes[i], 1);
        if (bit_val)
            KBD_KEY_BIT_SET(conn->keys, i);
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

    // Check for no keys pressed.
    bool any_key = false;
    kbd_keys[0] &= ~0xF;
    for (int k = 0; k < 8; k++)
        if (kbd_keys[k])
            any_key = true;
    if (!any_key)
        kbd_keys[0] |= 1;

    // NUMLOCK CAPSLOCK SCROLLLOCK
    kbd_keys[0] |= (kbd_hid_leds & 7) << 1;

    // Send it to xram
    if (kbd_xram != 0xFFFF)
        memcpy((uint8_t *)&xram[kbd_xram], kbd_keys, sizeof(kbd_keys));
}

bool kbd_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - sizeof(kbd_keys))
        return false;
    kbd_xram = word;
    if (kbd_xram != 0xFFFF)
        memcpy((uint8_t *)&xram[kbd_xram], kbd_keys, sizeof(kbd_keys));
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
}

