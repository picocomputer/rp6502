/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Typing on a machine whose OS has already decoded the key. What a host types
 * goes into the typed queue; see core/hid/typed.h.
 *
 * Which keys are down is core/hid/keyboard.h. A block of text is
 * core/hid/paste_host.h.
 */

#ifndef _CORE_HID_HOSTKEY_H_
#define _CORE_HID_HOSTKEY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What hostkey_type, or hostkey_press, did with a key. */
typedef enum
{
    HOSTKEY_NONE,  /* the key sends nothing */
    HOSTKEY_TYPED, /* the key's bytes went to the console */
    HOSTKEY_CHORD, /* a chord of core/hid/key.h acted, or is held */
} hostkey_result_t;

/* Type a key press as vtkey_encode has it, unless key_chord takes it. A
 * cut key is a chord that types what it would, behind its mark.
 * modifier is a HID report's modifier byte, KEYBOARD_MODIFIER_ bits, and
 * repeat says the key is held and the host is repeating it.
 *
 * A key with a character types it here only when guess is set, which a host
 * sets for a press its system gives no text for, such as a Ctrl or Alt
 * chord. The character is then a US keyboard's, by Shift and Caps Lock.
 * Otherwise the host delivers the text through hostkey_char. */
hostkey_result_t hostkey_press(uint8_t usage, uint8_t modifier, bool repeat, bool guess);

/* The press itself, for a caller that knows the key's character: ch is
 * typed where the terminal's encoding is a character, and 0 types nothing
 * there, for a host that delivers its text through hostkey_char. meta says
 * Alt goes with the character as Meta, ESC before it; otherwise Alt is taken
 * to have chosen the character, as AltGr does. hostkey_press is this with a
 * guessed character, and Alt always Meta. */
hostkey_result_t hostkey_type(uint8_t usage, uint8_t modifier, bool repeat, char ch, bool meta);

/* A US keyboard's character for a key, by Shift and Caps Lock, for a press
 * the host's system gives no text for, such as a Ctrl or Alt chord. */
char hostkey_guess(uint8_t usage, uint8_t modifier, bool capslock);

/* Where typed bytes go: type a key's bytes, and cut a cut key's behind its
 * mark (core/hid/typed.h). The typed queue's own, unless a machine's
 * keyboard driver composes them first, as keymap.c does its dead keys. NULL
 * puts back the queue's. */
typedef struct
{
    void (*type)(const char *seq, size_t n);
    void (*cut)(const char *seq, size_t n);
} hostkey_typer_t;
void hostkey_set_typer(const hostkey_typer_t *typer);

/* The character a key has on a US keyboard, or 0, and the key and Shift that
 * type a character there, or 0. */
char hostkey_us(uint8_t usage, bool shift);
uint8_t hostkey_us_key(char ch, bool *shift);

/* Type one Unicode code point as its byte in the active code page. */
void hostkey_char(uint32_t codepoint);

#endif /* _CORE_HID_HOSTKEY_H_ */
