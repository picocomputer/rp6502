/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The key chords every machine shares, which act instead of typing. Every
 * key is typed through core/hid/hostkey.c, which asks here before the key is
 * encoded, whether the firmware's layout gave its character
 * (core/hid/keymap.c) or a host's OS did. A chord only a keyboard driver
 * has, such as GUI-Space for the next layout or a lock key, stays with that
 * driver. */

#ifndef _CORE_HID_KEY_H_
#define _CORE_HID_KEY_H_

#include <stdbool.h>
#include <stdint.h>

/* The copy, cut and paste keys (clip.h), which are chords whatever a
 * program has asked of the terminal. Each acts once for a press and never
 * repeats:
 *
 * - paste: Ctrl-Shift-V and Shift-Insert, and on a Mac Cmd-V
 *   (host_mac_keys), paste the clipboard. The key types nothing, and the
 *   text comes in its place among the keys (typed.h);
 * - copy: Ctrl-Shift-C and Ctrl-Insert, and on a Mac Cmd-C. The key types
 *   nothing, so Ctrl-Shift-C raises no SIGINT. While the mouse's highlight
 *   shows on the screen, the text it shows goes to the clipboard at the
 *   press. Otherwise the copy waits in its place among the keys for the
 *   line editor, which copies the line's selection (core/com/rln.h), and
 *   with no line being read it does nothing;
 * - cut: Shift-Delete and Ctrl-Shift-X, and on a Mac Cmd-X. The key is the
 *   program's: it types what it would, behind a mark. The line editor
 *   takes the mark and the bytes, and cuts the line's selection. With no
 *   line being read the mark goes and the bytes are typed.
 *
 * Plain Ctrl-C and Ctrl-V stay the program's. */
typedef enum
{
    KEY_CLIP_NONE,
    KEY_CLIP_COPY,
    KEY_CLIP_CUT,
    KEY_CLIP_PASTE,
} key_clip_t;

/* Which of them a press is. A web page reads its clipboard only from the
 * paste event a paste key makes, so its host asks this to let the key make
 * one, and to keep the copy and cut keys from the browser. */
key_clip_t key_clip(uint8_t keycode, uint8_t modifier);

/* What a key press with the modifiers held is, as a HID report's modifier
 * byte of KEYBOARD_MODIFIER_ bits. */
typedef enum
{
    KEY_CHORD_NONE,  /* no chord: the key types */
    KEY_CHORD_ACTED, /* a chord that acted, or is held: the key types nothing */
    KEY_CHORD_CUT,   /* a cut key: what the key types goes to typed_cut */
} key_chord_t;

/* A repeat of a key whose press was a chord does nothing and has acted, so
 * a held chord acts once. */
key_chord_t key_chord(uint8_t keycode, uint8_t modifier, bool repeat);

#endif /* _CORE_HID_KEY_H_ */
