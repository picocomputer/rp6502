/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_SYS_KBD_H_
#define _CORE_SYS_KBD_H_

#include <stdbool.h>
#include <stdint.h>

#include "core/hid/kbd.h"

/* Non-character keys, mapped to the same xterm/VT byte sequences the
 * firmware's terminal half emits (core/hid/kbt.c). Printable keys arrive as
 * UTF-8 text via kbd_text() instead. */
typedef enum
{
    KBD_KEY_ENTER,
    KBD_KEY_BACKSPACE,
    KBD_KEY_TAB,
    KBD_KEY_ESCAPE,
    KBD_KEY_UP,
    KBD_KEY_DOWN,
    KBD_KEY_LEFT,
    KBD_KEY_RIGHT,
    KBD_KEY_HOME,
    KBD_KEY_END,
    KBD_KEY_INSERT,
    KBD_KEY_DELETE,
    KBD_KEY_PAGE_UP,
    KBD_KEY_PAGE_DOWN,
    KBD_KEY_F1,
    KBD_KEY_F2,
    KBD_KEY_F3,
    KBD_KEY_F4,
    KBD_KEY_F5,
    KBD_KEY_F6,
    KBD_KEY_F7,
    KBD_KEY_F8,
    KBD_KEY_F9,
    KBD_KEY_F10,
    KBD_KEY_F11,
    KBD_KEY_F12,
} kbd_key_t;

/* The ANSI keyboard stream. Everything a host types enters here — nothing else
 * pushes the com keyboard ring (see emu/sys/com.h). */

/* Queue printable UTF-8 input as OEM bytes, converting each sequence to the
 * active code page. */
void kbd_text(const char *utf8);

/* Queue a non-character key as its xterm sequence, annotating shift/alt/ctrl
 * into the modifier number the way xterm/the firmware do. */
void kbd_key(kbd_key_t key, bool ctrl, bool shift, bool alt);

/* Queue a Ctrl+<letter> chord as its C0 control byte. A byte outside the
 * promotable @.._ / `..~ range is not a chord and queues nothing. */
void kbd_ctrl_letter(char letter);

/* Queue Alt+<char> as the xterm Meta form: ESC then the byte, ctrl-promoted
 * first when both are held (the firmware's order). */
void kbd_alt_char(char ch, bool ctrl);

/* Type a block of UTF-8 as keystrokes: printable runs as text, CR/LF/CRLF as one
 * Enter, tab as Tab, other control bytes stripped. Delivery is paced by kbd_task
 * against the ring's headroom, so text longer than the ring cannot lose bytes.
 * A new call replaces whatever is still dripping. */
void kbd_paste(const char *utf8);
void kbd_paste_cancel(void);

/* True while a paste is still being handed to the ring. */
bool kbd_paste_busy(void);

/* Per-frame service: the paste drip. Called from sys_run_frame so the window,
 * the headless batch and a script all pace a paste identically. */
void kbd_task(void);

/* Key names, for callers that take them as text rather than as an enum:
 * "a", "7", "up", "f5", "space", "lshift". kbd_hid_from_name answers with the
 * USB HID usage id kbd_hid_set wants (0 = unknown); kbd_key_from_name answers
 * only for the keys that also have an xterm sequence. */
uint8_t kbd_hid_from_name(const char *name);
bool kbd_key_from_name(const char *name, kbd_key_t *key);

#endif /* _CORE_SYS_KBD_H_ */
