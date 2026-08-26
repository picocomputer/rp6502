/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_KEYBOARD_H_
#define _CORE_HID_KEYBOARD_H_

/* Which keys are down, as a bitmap the 6502 polls. What that spells is
 * core/hid/keymap.h.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "core/hid/hid.h"

/* Main events
 */

/* A run of consecutive keyboard usages, one bit each -- how both the boot
 * keyboard's modifier byte and an NKRO bitmap are declared. */
#define KEYBOARD_KEY_RUNS 4
typedef struct
{
    uint16_t bit_pos;  // bit of usage_min
    uint16_t usage_min;
    uint16_t count;    // 0 ends the list
} keyboard_key_run_t;

#define KEYBOARD_MAX_KEYBOARDS 4

/* One keyboard, as the driver reads it. A descriptor is parsed into this;
 * a machine with no descriptor to parse writes one out. */
typedef struct keyboard_connection
{
    bool valid;
    int slot;              // HID slot
    uint32_t keys[8];      // last report, bits 0-3 unused
    uint8_t report_id;     // If non zero, the first report byte must match and will be skipped
    uint16_t codes_offset; // Offset in bits for keycode array
    uint8_t codes_count;   // Number of keycodes in array
    keyboard_key_run_t runs[KEYBOARD_KEY_RUNS]; // one-bit-per-usage keys
} keyboard_connection_t;

void keyboard_init(void);
void keyboard_stop(void);

// Claim this device, if its map describes a keyboard.
bool keyboard_mount(int slot, const keyboard_connection_t *desc,
                    uint16_t vendor_id, uint16_t product_id);

// Clean up descriptor when device is disconnected.
bool keyboard_umount(int slot);

// Process HID keyboard report.
void keyboard_report(int slot, uint8_t const *data, size_t size);

// Report ID of the keyboard on this slot, or 0 if its report map uses none.
uint8_t keyboard_get_report_id(int slot);

// Set the extended register value.
bool keyboard_xreg(uint16_t word);

/* What the terminal half needs back: the merged modifier byte, whether a
 * key is still held (auto-repeat asks), and the lock lamps, which live here
 * because they also ride in the bitmap the 6502 reads. */
uint8_t keyboard_get_modifier(void);
bool keyboard_key_down(uint8_t keycode);
uint8_t keyboard_get_leds(void);
void keyboard_toggle_lock(uint8_t bit);

// A host that decodes its own keyboard, in place of a report.
void keyboard_hid_set(uint8_t keycode, bool down);

/* What a keypad key navigates to with NumLock off: KP7 is Home, KP2 is Down,
 * KP5 is nowhere. Zero for any usage that is not on the keypad, and for KP5.
 * A keyboard fact, so every machine reads the same one. */
uint8_t keyboard_keypad_nav(uint8_t hid_usage);

/* Offered to whatever spells for this machine: every new key press, and the
 * modifier byte after every report. A firmware's layout engine answers these;
 * a machine whose host produced the characters before the keystroke arrived
 * answers with nothing and takes the text instead. */
void keyboard_spell_key(uint8_t modifier, uint8_t keycode);
void keyboard_spell_modifiers(uint8_t modifier);

#endif /* _CORE_HID_KEYBOARD_H_ */
