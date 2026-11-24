/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Typing on a machine whose OS has already decoded the key. What a host types
 * goes into the typed queue; see core/hid/typed.h.
 *
 * Which keys are down is core/hid/keyboard.h. A block of text is
 * core/hid/paste.h.
 */

#ifndef _CORE_HID_HOSTKEY_H_
#define _CORE_HID_HOSTKEY_H_

#include <stdbool.h>
#include <stdint.h>

/* What hostkey_press did with a key. */
typedef enum
{
    HOSTKEY_NONE,  /* the key sends nothing */
    HOSTKEY_TYPED, /* the key's bytes went to the console */
    HOSTKEY_CHORD, /* a chord of core/hid/key.h acted, or is held */
} hostkey_result_t;

/* Type a key press as vtkey_encode has it, unless key_chord takes it.
 * modifier is a HID report's modifier byte, KEYBOARD_MODIFIER_ bits, and
 * repeat says the key is held and the host is repeating it.
 *
 * A key with a character types it here only when guess is set, which a host
 * sets for a press its system gives no text for, such as a Ctrl or Alt
 * chord. The character is then a US keyboard's, by Shift and Caps Lock.
 * Otherwise the host delivers the text through hostkey_char. */
hostkey_result_t hostkey_press(uint8_t usage, uint8_t modifier, bool repeat, bool guess);

/* The character a key has on a US keyboard, or 0, and the key and Shift that
 * type a character there, or 0. */
char hostkey_us(uint8_t usage, bool shift);
uint8_t hostkey_us_key(char ch, bool *shift);

/* Type one Unicode code point as its byte in the active code page. */
void hostkey_char(uint32_t codepoint);

#endif /* _CORE_HID_HOSTKEY_H_ */
