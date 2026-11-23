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

/* The copy and paste keys (clip.h), which are chords whatever a program has
 * asked of the terminal, so they never type, and Ctrl-Shift-C raises no
 * SIGINT:
 *
 * - copy: Ctrl-Shift-C and Ctrl-Insert, and on a Mac Cmd-C
 *   (host_mac_keys), put the primary in the clipboard;
 * - paste: Ctrl-Shift-V and Shift-Insert, and on a Mac Cmd-V, paste the
 *   clipboard.
 *
 * Plain Ctrl-C and Ctrl-V stay the program's. */
typedef enum
{
    KEY_CLIP_NONE,
    KEY_CLIP_COPY,
    KEY_CLIP_PASTE,
} key_clip_t;

/* Which of them a press is. A web page reads its clipboard only from the
 * paste event a paste key makes, so its host asks this to let the key make
 * one. */
key_clip_t key_clip(uint8_t keycode, uint8_t modifier);

#endif /* _CORE_HID_KEY_H_ */
