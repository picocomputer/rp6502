/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The key chords every machine shares, which act instead of typing. Both
 * keyboard paths ask here before a key is encoded: core/hid/keymap.c where
 * the firmware decodes keys, and core/hid/hostkey.c where a host's OS has.
 * A chord only a keyboard driver has, such as GUI-Space for the next layout
 * or a lock key, stays with that driver. */

#ifndef _CORE_HID_KEY_H_
#define _CORE_HID_KEY_H_

#include <stdbool.h>
#include <stdint.h>

/* A key press with the modifiers held, as a HID report's modifier byte of
 * KEYBOARD_MODIFIER_ bits. True when it is a chord that acted, and then the
 * key types nothing. A repeat of a key whose press acted does nothing and is
 * true too, so a held chord acts once. */
bool key_chord(uint8_t keycode, uint8_t modifier, bool repeat);

#endif /* _CORE_HID_KEY_H_ */
