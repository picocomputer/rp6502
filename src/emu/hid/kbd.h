/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Keyboard input (kbd.c) — replaces the firmware USB HID driver. Two faces: the
 * stdin byte stream (printable text + xterm/VT sequences for the line editor)
 * and the HID keyboard bitmap (the xreg_ria_keyboard API).
 */

#ifndef _EMU_KBD_H_
#define _EMU_KBD_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Non-character keys, mapped to the same xterm/VT byte sequences the firmware
 * USB HID driver emits (ria/hid/kbd.c). Printable keys arrive as UTF-8 text via
 * kbd_text() instead. The window layer (app_sokol) translates host key events
 * into these; tests/headless inject directly. */
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

/* Queue printable input as OEM bytes: each UTF-8 sequence is converted to the
 * active code page (a CHAR event's character, or a scripted line). */
void kbd_text(const char *utf8);

/* Queue a non-character key as its xterm sequence, annotating shift/alt/ctrl
 * into the modifier number the way xterm/the firmware do. */
void kbd_key(kbd_key_t key, bool ctrl, bool shift, bool alt);

/* Queue a Ctrl+<letter> chord as its C0 control byte, latching SIGINT on
 * Ctrl-C at the keyboard (the firmware HID driver's early break detection). */
void kbd_ctrl_letter(char letter);

/* HID keyboard bitmap (the xreg_ria_keyboard API), separate from the stdin
 * stream above. kbd_set_xram points it at an XRAM address (0xFFFF = off);
 * kbd_hid_set toggles a HID keycode's bit. Mirrors ria/hid/kbd.c: word 0
 * bit 0 reads 1 when no keys are down. */
bool kbd_set_xram(uint16_t addr);
void kbd_hid_set(uint8_t hid_keycode, bool down);
void kbd_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_KBD_H_ */
