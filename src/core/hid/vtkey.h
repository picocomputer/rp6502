/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What a key sends to the console. Every machine types through this, so a
 * key sends the same bytes whether a layout or a host's OS decoded it. */

#ifndef _CORE_HID_VTKEY_H_
#define _CORE_HID_VTKEY_H_

#include <stdbool.h>
#include <stdint.h>

/* The modifiers held with a key. One more than these bits is the modifier
 * number xterm puts in a sequence. The GUI key is not one: a key sends the
 * same with it held. */
#define VTKEY_SHIFT 1
#define VTKEY_ALT 2
#define VTKEY_CTRL 4
#define VTKEY_MODIFIERS 7

/* The longest sequence, ESC ESC [ 2 7 ; 8 ; 2 5 5 ~. */
#define VTKEY_MAX 13

/* vtkey_encode's answer for a key that has a character, which the machine's
 * layout or host knows. */
#define VTKEY_CHAR (-1)

/* What a key sends, written with no NUL, in the modes the console's terminal
 * is in and by the keyboard's NumLock. Returns the length, 0 for a key that
 * sends nothing, or VTKEY_CHAR.
 *
 * Enter is CR, or CR LF in new line mode (LNM). Tab is HT, or ESC [ Z with
 * Shift. Escape is ESC and Backspace is DEL, or BS with Ctrl. Alt puts ESC
 * in front of those.
 *
 * The cursor keys, Home and End send ESC [ A, or ESC O A in cursor key mode
 * (DECCKM), and F1 to F4 send ESC O P. With a modifier they are
 * ESC [ 1 ; mod A. The other function and navigation keys send
 * ESC [ number ~. VT52 mode changes only the keys a VT52 has: the ones with
 * a letter send ESC and the letter, whatever the modifiers. The others send
 * what they always do, without modifiers.
 *
 * The keypad with NumLock on types its characters, and Shift there is the
 * same as NumLock off, with Shift used up. With NumLock off the application
 * keypad (DECKPAM) sends a VT100's keypad, ESC O p and so on and ESC ? p in
 * VT52 mode, with the modifier number before the letter as xterm has it.
 * Plus and Keypad Comma are both the VT100's comma; divide and multiply have
 * xterm's letters. Otherwise the digits are their navigation keys, where
 * Keypad 5 is xterm's Begin, ESC [ E, the operators type their characters
 * and Keypad Comma types a comma. Keypad Enter is Enter, without Alt, except
 * on the application keypad.
 *
 * xterm's modifyOtherKeys (CSI > 4 ; level m) sends a key with no sequence
 * of its own as ESC [ 27 ; mod ; code ~, where code is the character it
 * types, or as ESC [ code ; mod u with formatOtherKeys (CSI > 4 ; 1 f).
 * Level 1 does so only for what has no byte of its own, such as Ctrl-Shift
 * with a letter, Ctrl with a digit, and Ctrl or Shift with Enter. Level 2
 * does so for every modified key, Shift with a letter included. Level 3
 * does so for every key. The cursor, function, editing and keypad keys send
 * what they always do, and VT52 mode has none of this. */
int vtkey_encode(char out[VTKEY_MAX], uint8_t usage, uint8_t flags);

/* What a key's character sends, written with no NUL; returns the length, 0
 * for no character. Ctrl makes the control byte, as xterm makes it: Ctrl-@
 * through Ctrl-_ and Ctrl-` through Ctrl-~ are 0x00 to 0x1F, Space and 2 are
 * 0x00, 3 to 7 are ESC to 0x1F, 8 and ? are DEL and / is 0x1F; any other
 * character is typed as it is. Alt puts ESC in front. A keypad key's
 * character is typed whatever Ctrl and Alt say. */
int vtkey_char(char out[VTKEY_MAX], uint8_t usage, char ch, uint8_t flags);

/* Whether a held key repeats (DECARM). A machine asks before it types a
 * repeat; the first press is always typed. */
bool vtkey_repeats(void);

#endif /* _CORE_HID_VTKEY_H_ */
