/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_SYS_KEYBOARD_H_
#define _CORE_SYS_KEYBOARD_H_

#include <stdbool.h>
#include <stdint.h>

#include "core/hid/keyboard.h"

/* The ANSI keyboard stream. Everything a host types enters here — nothing else
 * pushes the com keyboard ring (see core/com/com.h). */

/* Queue printable UTF-8 input as OEM bytes, converting each sequence to the
 * active code page. */
void keyboard_text(const char *utf8);

/* Queue a non-character key as its xterm sequence, annotating shift/alt/ctrl
 * into the modifier number the way xterm/the firmware do. */
bool keyboard_key(uint8_t hid_usage, bool ctrl, bool shift, bool alt);

/* Queue a Ctrl+<letter> chord as its C0 control byte. A byte outside the
 * promotable @.._ / `..~ range is not a chord and queues nothing. */
void keyboard_ctrl_letter(char letter);

/* Queue Alt+<char> as the xterm Meta form: ESC then the byte, ctrl-promoted
 * first when both are held (the firmware's order). */
void keyboard_alt_char(char ch, bool ctrl);

/* Type a block of UTF-8 as keystrokes: printable runs as text, CR/LF/CRLF as one
 * Enter, tab as Tab, other control bytes stripped. Delivery is paced by keyboard_task
 * against the ring's headroom, so text longer than the ring cannot lose bytes.
 * A new call replaces whatever is still dripping. */
void keyboard_paste(const char *utf8);
void keyboard_paste_cancel(void);

/* True while a paste is still being handed to the ring. */
bool keyboard_paste_busy(void);

/* Per-frame service: the paste drip. Called from sys_run_frame so the window,
 * the headless batch and a script all pace a paste identically. */
void keyboard_task(void);

/* Key names, for callers that take them as text rather than as an enum:
 * "a", "7", "up", "f5", "space", "lshift". keyboard_hid_from_name answers with the
 * USB HID usage id keyboard_hid_set and keyboard_key both want (0 = unknown), which
 * only for the keys that also have an xterm sequence. */
uint8_t keyboard_hid_from_name(const char *name);

#endif /* _CORE_SYS_KEYBOARD_H_ */
