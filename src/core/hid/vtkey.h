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
 * number xterm puts in a sequence. */
#define VTKEY_SHIFT 1
#define VTKEY_ALT 2
#define VTKEY_CTRL 4
#define VTKEY_GUI 8

/* NumLock is on. */
#define VTKEY_NUMLOCK 16

/* The longest sequence, ESC [ 2 4 ; 1 6 ~. */
#define VTKEY_MAX 8

/* vtkey_encode's answer for a key that has a character, which the machine's
 * layout or host knows. */
#define VTKEY_CHAR (-1)

/* What a key sends, written with no NUL, in the modes the console's terminal
 * is in. Returns the length, 0 for a key that sends nothing, or VTKEY_CHAR.
 *
 * Enter is CR, or CR LF in new line mode (LNM). Tab is HT, Escape is ESC and
 * Backspace is DEL, or BS with Ctrl. Alt puts ESC in front of those, but not
 * in VT52 mode.
 *
 * The cursor keys, Home and End send ESC [ A, or ESC O A in cursor key mode
 * (DECCKM), and F1 to F4 send ESC O P. With a modifier they are
 * ESC [ 1 ; mod A. The other function and navigation keys send
 * ESC [ number ~. In VT52 mode the keys with a letter send ESC and the
 * letter, whatever the modifiers, and the others send nothing.
 *
 * The keypad follows xterm. With NumLock on it types its characters, in
 * either keypad mode, and Shift makes a digit the navigation key printed
 * under it. With NumLock off the application keypad (DECKPAM) sends ESC O p
 * and so on, ESC ? p in VT52 mode; the numeric keypad's digits are their
 * navigation keys, where Keypad 5 sends nothing, and its operators type their
 * characters. Keypad Enter is Enter except on the application keypad. */
int vtkey_encode(char out[VTKEY_MAX], uint8_t usage, uint8_t flags);

/* What Ctrl makes of a character, as xterm and the Linux console make it:
 * Ctrl-@ through Ctrl-_ and Ctrl-` through Ctrl-~ are 0x00 to 0x1F, Space and
 * 2 are 0x00, 3 to 7 are ESC to 0x1F, 8 is DEL and / is 0x1F. A byte that is
 * C0 already types itself. -1 when Ctrl has nothing to say, which a ch of 0,
 * meaning no character, always gets. */
int vtkey_ctrl(char ch);

/* What a key's character sends, written with no NUL; returns the length, 0
 * for nothing. Ctrl makes the control byte and Alt puts ESC in front, where
 * a character Ctrl has nothing to say about is still typed. Ctrl alone on
 * such a character sends nothing. A VT52 has no Alt. */
int vtkey_char(char out[VTKEY_MAX], char ch, uint8_t flags);

/* Whether a held key repeats (DECARM). A machine asks before it types a
 * repeat; the first press is always typed. */
bool vtkey_repeats(void);

#endif /* _CORE_HID_VTKEY_H_ */
