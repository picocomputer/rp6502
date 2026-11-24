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

/* The copy, cut and paste keys (clip.h). The copy and paste keys are
 * chords whatever a program has asked of the terminal, so they never type,
 * and Ctrl-Shift-C raises no SIGINT. Each key is judged at once, against
 * what the screen shows at that moment:
 *
 * - copy: Ctrl-Shift-C and Ctrl-Insert, and on a Mac Cmd-C
 *   (host_mac_keys), put the line editor's selection in the clipboard
 *   (core/com/rln.h), or with none, the primary, at once;
 * - cut: Shift-Delete and Ctrl-Shift-X, and on a Mac Cmd-X, with the
 *   line editor's selection showing, cut it to the clipboard; with none
 *   they are no chord, and type as they would;
 * - paste: Ctrl-Shift-V and Shift-Insert, and on a Mac Cmd-V, paste the
 *   clipboard, at once, or with the line editor's selection showing, in
 *   its place.
 *
 * The line editor takes a cut or paste of its selection after the keys
 * typed before it, as it takes the mouse. Plain Ctrl-C and Ctrl-V stay the
 * program's. */
typedef enum
{
    KEY_CLIP_NONE,
    KEY_CLIP_COPY,
    KEY_CLIP_CUT,
    KEY_CLIP_PASTE,
} key_clip_t;

/* Which of them a press is. A web page reads its clipboard only from the
 * paste event a paste key makes, so its host asks this to let the key make
 * one. */
key_clip_t key_clip(uint8_t keycode, uint8_t modifier);

/* A copy, cut or paste key acts, and true when it did. A web page, whose
 * paste keys are the browser's, acts with the paste event they make. */
bool key_clip_act(key_clip_t clip);

/* Whether a press of the key, or its repeat, is a copy, cut or paste key
 * that acts now, as key_chord would judge it: a copy or paste key always,
 * a cut key only with the line editor's selection showing, and a cut
 * key's repeat when its press acted. The web player withholds a copy or
 * cut key that acts from the browser (host/sokol/app/input.h). */
bool key_clip_acts(uint8_t keycode, uint8_t modifier, bool repeat);

#endif /* _CORE_HID_KEY_H_ */
