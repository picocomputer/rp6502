/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/xram.h"
#include "core/hid/keyboard.h"
#include "core/hid/hid.h"
#include "machine.h"
#include "core/hid/usage.h"
#include <string.h>

static uint16_t keyboard_xram;
static uint8_t keyboard_locks;
static uint32_t keyboard_keys[8];
static keyboard_connection_t keyboard_connections[KEYBOARD_MAX_KEYBOARDS];

/* An address that would put the bitmap past the end of XRAM would have the
 * next publish write past it. */
static bool keyboard_xram_check(sst_cursor_t *c)
{
    if (keyboard_xram > 0x10000 - sizeof(keyboard_keys) &&
        keyboard_xram != 0xFFFF)
        return sst_refuse(c, "keyboard block past the end of XRAM");
    return true;
}

/* The connection table is not saved, because it describes what is plugged
 * in now rather than what the blob remembers. */
#define KEYBOARD_SST(X)                                     \
    X(U16, keyboard_xram)                                   \
    X(U8, keyboard_locks)                                   \
    X(U32S, keyboard_keys)                                  \
    X(VALIDATE, keyboard_xram_check)                        \
    X(SKIP, keyboard_connections)
SST_TABLE(keyboard, KEYBOARD_SST)

#define KEYBOARD_KEY_BIT_SET(data, keycode) ((data)[(keycode) >> 5] |= 1 << ((keycode) & 31))
#define KEYBOARD_KEY_BIT_VAL(data, keycode) ((data)[(keycode) >> 5] & (1 << ((keycode) & 31)))

/* The eight modifier usages run 0xE0 to 0xE7, so they fill the one bitmap
 * byte that holds HID_KEY_CONTROL_LEFT, in the order a HID report's modifier
 * byte uses. */
#define KEYBOARD_MODIFIER(keys) ((uint8_t *)keys)[HID_KEY_CONTROL_LEFT >> 3]

static keyboard_connection_t *HOST_IN_RAM_FUNC keyboard_get_connection_by_slot(int slot)
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

static void HOST_IN_RAM_FUNC keyboard_merge_keys(void)
{
    memset(keyboard_keys, 0, sizeof(keyboard_keys));
    for (int k = 0; k < 8; k++)
        for (int i = 0; i < KEYBOARD_MAX_KEYBOARDS; i++)
            keyboard_keys[k] |= keyboard_connections[i].keys[k];
}

/* Word 0's low four bits are not keys. Bit 0 says nothing is pressed and bits
 * 1 to 3 are the locks, so both are restated every time the block is
 * published. */
static void HOST_IN_RAM_FUNC keyboard_publish(void)
{
    bool any_key = false;
    keyboard_keys[0] &= ~0xF;
    for (int k = 0; k < 8; k++)
        if (keyboard_keys[k])
            any_key = true;
    if (!any_key)
        keyboard_keys[0] |= 1;
    keyboard_keys[0] |= (keyboard_locks & 7) << 1; // NUMLOCK CAPSLOCK SCROLLLOCK
    if (keyboard_xram != 0xFFFF)
        memcpy((uint8_t *)&xram[keyboard_xram], keyboard_keys, sizeof(keyboard_keys));
}

void HOST_IN_FLASH_FUNC keyboard_init(void)
{
    keyboard_stop();
    keyboard_locks = 0;
}

void keyboard_stop(void)
{
    keyboard_xram = 0xFFFF;
}

void keyboard_reset(void)
{
    keyboard_stop();
    keyboard_release_all();
}

bool keyboard_mount(int slot, const keyboard_connection_t *desc,
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
        keyboard_on_mount(vendor_id, product_id);
        return true;
    }
    return false;
}

bool keyboard_umount(int slot)
{
    keyboard_connection_t *conn = keyboard_get_connection_by_slot(slot);
    if (conn == NULL)
        return false;
    conn->valid = false;
    memset(conn->keys, 0, sizeof(conn->keys));
    keyboard_merge_keys();
    keyboard_publish();
    return true;
}

void HOST_IN_RAM_FUNC keyboard_report(int slot, uint8_t const *data, size_t size)
{
    keyboard_connection_t *conn = keyboard_get_connection_by_slot(slot);
    if (conn == NULL)
        return;

    const uint8_t *report_data = data;
    uint16_t report_data_len = size;

    if (conn->report_id != 0)
    {
        if (report_data_len == 0 || report_data[0] != conn->report_id)
            return;
        report_data++;
        report_data_len--;
    }

    uint32_t old_keys[8];
    memcpy(old_keys, conn->keys, sizeof(conn->keys));
    memset(conn->keys, 0, sizeof(conn->keys));

    for (int i = 0; i < conn->codes_count; i++)
    {
        uint16_t bit_offset = conn->codes_offset + (i * 8);
        uint8_t keycode = (uint8_t)hid_extract_bits(report_data, report_data_len,
                                                    bit_offset, 8);
        if (keycode == 1)
        {
            /* Usage 1 is ErrorRollOver: the keyboard has more keys down than
             * it can report, and this report says nothing about any of them. */
            memcpy(conn->keys, old_keys, sizeof(conn->keys));
            return;
        }
        KEYBOARD_KEY_BIT_SET(conn->keys, keycode);
    }

    for (int r = 0; r < KEYBOARD_KEY_RUNS; r++)
    {
        const keyboard_key_run_t *run = &conn->runs[r];
        if (!run->count)
            break;
        for (uint16_t i = 0; i < run->count; i++)
            if (hid_extract_bits(report_data, report_data_len, run->bit_pos + i, 1))
                KEYBOARD_KEY_BIT_SET(conn->keys, run->usage_min + i);
    }

    /* The merge comes first because the machine's hook is handed the merged
     * modifier byte, and a modifier pressed in this same report has to be in
     * it. */
    keyboard_merge_keys();

    /* Every key below the modifiers, which have the hook after this one. */
    for (int i = 0; i < HID_KEY_CONTROL_LEFT; i++)
    {
        bool curr = KEYBOARD_KEY_BIT_VAL(conn->keys, i);
        bool prev = KEYBOARD_KEY_BIT_VAL(old_keys, i);
        if (curr && !prev)
            keyboard_on_press(KEYBOARD_MODIFIER(keyboard_keys), i);
    }

    keyboard_on_modifiers(KEYBOARD_MODIFIER(keyboard_keys));

    keyboard_publish();
}

bool HOST_IN_FLASH_FUNC keyboard_xreg(uint16_t word)
{
    bool mapped = word <= 0x10000 - sizeof(keyboard_keys);
    keyboard_xram = mapped ? word : 0xFFFF;
    keyboard_publish();
    return mapped || word == 0xFFFF;
}

bool keyboard_is_mapped(void)
{
    return keyboard_xram != 0xFFFF;
}

uint8_t HOST_IN_RAM_FUNC keyboard_get_modifier(void)
{
    return KEYBOARD_MODIFIER(keyboard_keys);
}

bool HOST_IN_RAM_FUNC keyboard_key_down(uint8_t keycode)
{
    return KEYBOARD_KEY_BIT_VAL(keyboard_keys, keycode) != 0;
}

uint8_t HOST_IN_RAM_FUNC keyboard_get_locks(void)
{
    return keyboard_locks;
}

void HOST_IN_RAM_FUNC keyboard_set_locks(uint8_t locks)
{
    locks &= KEYBOARD_LED_NUMLOCK | KEYBOARD_LED_CAPSLOCK | KEYBOARD_LED_SCROLLLOCK;
    if (locks == keyboard_locks)
        return;
    keyboard_locks = locks;
    keyboard_publish();
}

void keyboard_release_all(void)
{
    memset(keyboard_keys, 0, sizeof(keyboard_keys));
    keyboard_publish();
}

/* Usages 0 to 3 are None and the three error codes, and their bits in word 0
 * carry the no-keys and lock flags instead, so no key may touch them. */
void keyboard_set_key(uint8_t keycode, bool down)
{
    if (keycode < 4)
        return;
    if (down)
        KEYBOARD_KEY_BIT_SET(keyboard_keys, keycode);
    else
        keyboard_keys[keycode >> 5] &= ~(1u << (keycode & 31));
    keyboard_publish();
}

