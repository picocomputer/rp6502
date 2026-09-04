/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_KEYBOARD_H_
#define _CORE_HID_KEYBOARD_H_

/* Which keys are down, as a bitmap the 6502 polls, and the facts about a key
 * that no layout and no host gets a vote on: where the keypad navigates, the
 * escape sequence a function key sends, what Ctrl makes of a byte. What a key
 * types is core/hid/keymap.h.
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

/* The xterm modifier parameter: 1 with nothing held, +1 shift, +2 alt, +4
 * ctrl, +8 gui. A host whose window manager owns the gui key passes false
 * for it. */
int keyboard_vt_mod(bool shift, bool alt, bool ctrl, bool gui);

/* The escape sequence a key with no character of its own sends, chosen by HID
 * usage: the twelve function keys and the ten navigation keys. Zero for any
 * other usage, including Enter, Tab, Escape and Backspace -- those have
 * characters, and which character is the machine's to say. The numbering that
 * says F5 is 15 and F6 is 17 is a table nobody can check by eye, so it lives
 * once. Writes into the caller's buffer; who the bytes go to is the caller's,
 * because one queues them for the 6502 and the other pushes a console ring. */
size_t keyboard_vt_seq(char *out, size_t cap, uint8_t hid_usage, int ansi_mod);

/* What Ctrl makes of a byte: Ctrl-A is 0x01 through Ctrl-Z 0x1A and the
 * punctuation range with it, Backspace is BS where DEL was, and a byte that
 * is C0 already types itself. 0 when Ctrl has nothing to say. Backspace needs
 * the keycode because DEL is outside both promotable ranges; pass
 * HID_KEY_NONE where there is no key, only a character. */
char keyboard_ctrl_promote(char ch, uint8_t keycode);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define KEYBOARD_DRIVER DRIVER(keyboard_init, nul_task, nul_task, nul_run, keyboard_stop, nul_break, nul_config, nul_config)

#endif /* _CORE_HID_KEYBOARD_H_ */
