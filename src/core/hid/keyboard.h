/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_KEYBOARD_H_
#define _CORE_HID_KEYBOARD_H_

#include <stddef.h>
#include <stdint.h>
#include "core/sys/sst.h"
#include <stdbool.h>

#include "core/hid/hid.h"

/* A run of consecutive keyboard usages, one bit each. A boot keyboard's
 * modifier byte and an NKRO bitmap are both declared this way. */
#define KEYBOARD_KEY_RUNS 4
typedef struct
{
    uint16_t bit_pos;  // bit position of usage_min
    uint16_t usage_min;
    uint16_t count;    // 0 ends the list
} keyboard_key_run_t;

#define KEYBOARD_MAX_KEYBOARDS 4

typedef struct keyboard_connection
{
    bool valid;
    int slot;
    uint32_t keys[8];      // last report, one bit per HID usage
    uint8_t report_id;     // when non-zero, the report starts with this byte
    uint16_t codes_offset; // bit offset of the keycode array
    uint8_t codes_count;
    keyboard_key_run_t runs[KEYBOARD_KEY_RUNS];
} keyboard_connection_t;

void keyboard_init(void);
void keyboard_stop(void);
/* After a failed load the bitmap's address and its keys hold whatever the
 * load left. This unmaps the bitmap and releases every key, so no stuck
 * modifier reaches the next program. The locks are masked wherever they are
 * read, so they stay. */
void keyboard_reset(void);

bool keyboard_mount(int slot, const keyboard_connection_t *desc,
                    uint16_t vendor_id, uint16_t product_id);

bool keyboard_umount(int slot);

void keyboard_report(int slot, uint8_t const *data, size_t size);

/* The machine's hooks. keyboard_mount calls the first for a keyboard it
 * takes. keyboard_report calls the second on every new key press, with the
 * modifier byte all keyboards hold between them, and the third after every
 * report. A machine that is its own keyboard driver answers them in
 * core/hid/keymap.c; one whose host already decoded the key answers with
 * nothing. */
void keyboard_on_mount(uint16_t vendor_id, uint16_t product_id);
void keyboard_on_press(uint8_t modifier, uint8_t keycode);
void keyboard_on_modifiers(uint8_t modifier);

uint8_t keyboard_get_report_id(int slot);

bool keyboard_xreg(uint16_t word);

bool keyboard_is_mapped(void);

uint8_t keyboard_get_modifier(void);
bool keyboard_key_down(uint8_t keycode);

/* NumLock, CapsLock and ScrollLock, in KEYBOARD_LED_ bits. This file only
 * keeps them, for the program's bitmap and for what the keys type. Their
 * owner sets them: core/hid/keymap.c on a machine that is its own keyboard
 * driver, where a lock key flips its lock and its light, and otherwise the
 * host, from the system that drives its keyboard. They start off. */
uint8_t keyboard_get_locks(void);
void keyboard_set_locks(uint8_t locks);

void keyboard_set_key(uint8_t keycode, bool down);

/* Releases every key and leaves the locks alone. A host that has just
 * restored a savestate calls this because the keys the blob remembers were
 * released in a session this machine is no longer in, so no release event is
 * coming to clear them. A key that is still physically down comes back on its
 * next report. */
void keyboard_release_all(void);

extern const sst_table_t keyboard_sst;

#define KEYBOARD_DRIVER DRIVER(                                            \
    INIT(keyboard_init)                                                    \
    STOP(keyboard_stop)                                                    \
    RESET(keyboard_reset)                                                  \
    SST("KEYB", keyboard))

#endif /* _CORE_HID_KEYBOARD_H_ */
