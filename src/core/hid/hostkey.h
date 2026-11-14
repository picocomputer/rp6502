/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Typing on a machine whose OS has already decoded the key. What a host types
 * goes into the manifold's keyboard ring; see core/com/manifold.h.
 *
 * Which keys are down is core/hid/keyboard.h. A block of text is
 * core/hid/paste.h.
 */

#ifndef _CORE_HID_HOSTKEY_H_
#define _CORE_HID_HOSTKEY_H_

#include <stdint.h>

/* What hostkey_press did with a key. */
typedef enum
{
    HOSTKEY_NONE,  /* the key sends nothing */
    HOSTKEY_TYPED, /* the key's bytes went to the console */
    HOSTKEY_CHORD, /* Ctrl-Alt-Delete or Alt-F4 acted */
} hostkey_result_t;

/* Type a key press as vtkey_encode has it. flags are the VTKEY_ flags of
 * core/hid/vtkey.h.
 *
 * ch is the US character the host guessed for the key, or 0 when the key has
 * none or the host delivers its text through hostkey_char. Ctrl turns ch into
 * its control byte and Alt puts ESC in front, the xterm Meta form. A ch that
 * Ctrl can make nothing of sends nothing. */
hostkey_result_t hostkey_press(uint8_t usage, uint8_t flags, char ch);

/* Type one Unicode code point as its byte in the active code page. */
void hostkey_char(uint32_t codepoint);

#endif /* _CORE_HID_HOSTKEY_H_ */
