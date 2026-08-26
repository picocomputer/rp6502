/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The keyboard constants of the USB HID usage tables. They are the
 * specification's numbers, so they are the machine's rather than any one
 * host's: only the RIA has USB, and learning that Escape is 0x29 should not
 * cost a Pocket a USB stack.
 *
 * Only what core/hid/keyboard.c uses. The RIA build compares every one against
 * the vendored TinyUSB header (src/core/gen/hid_usage_check.py) because that is
 * the tree that has it -- the machines that need this file do not, which is
 * the whole reason it exists.
 */

#ifndef _CORE_HID_USAGE_H_
#define _CORE_HID_USAGE_H_

#define HID_KEY_NONE 0x00
#define HID_KEY_ENTER 0x28
#define HID_KEY_ESCAPE 0x29
#define HID_KEY_BACKSPACE 0x2A
#define HID_KEY_TAB 0x2B
#define HID_KEY_SPACE 0x2C
#define HID_KEY_CAPS_LOCK 0x39
#define HID_KEY_F1 0x3A
#define HID_KEY_F2 0x3B
#define HID_KEY_F3 0x3C
#define HID_KEY_F4 0x3D
#define HID_KEY_F5 0x3E
#define HID_KEY_F6 0x3F
#define HID_KEY_F7 0x40
#define HID_KEY_F8 0x41
#define HID_KEY_F9 0x42
#define HID_KEY_F10 0x43
#define HID_KEY_F11 0x44
#define HID_KEY_F12 0x45
#define HID_KEY_SCROLL_LOCK 0x47
#define HID_KEY_INSERT 0x49
#define HID_KEY_HOME 0x4A
#define HID_KEY_PAGE_UP 0x4B
#define HID_KEY_DELETE 0x4C
#define HID_KEY_END 0x4D
#define HID_KEY_PAGE_DOWN 0x4E
#define HID_KEY_ARROW_RIGHT 0x4F
#define HID_KEY_ARROW_LEFT 0x50
#define HID_KEY_ARROW_DOWN 0x51
#define HID_KEY_ARROW_UP 0x52
#define HID_KEY_NUM_LOCK 0x53
#define HID_KEY_KEYPAD_ENTER 0x58
#define HID_KEY_KEYPAD_1 0x59
#define HID_KEY_KEYPAD_2 0x5A
#define HID_KEY_KEYPAD_3 0x5B
#define HID_KEY_KEYPAD_4 0x5C
#define HID_KEY_KEYPAD_5 0x5D
#define HID_KEY_KEYPAD_6 0x5E
#define HID_KEY_KEYPAD_7 0x5F
#define HID_KEY_KEYPAD_8 0x60
#define HID_KEY_KEYPAD_9 0x61
#define HID_KEY_KEYPAD_0 0x62
#define HID_KEY_KEYPAD_DECIMAL 0x63
#define HID_KEY_CONTROL_LEFT 0xE0

/* The modifier byte of a keyboard report, and the lock lamps of its
 * output report. */
#define KEYBOARD_MODIFIER_LEFTCTRL 0x01
#define KEYBOARD_MODIFIER_LEFTSHIFT 0x02
#define KEYBOARD_MODIFIER_LEFTALT 0x04
#define KEYBOARD_MODIFIER_LEFTGUI 0x08
#define KEYBOARD_MODIFIER_RIGHTCTRL 0x10
#define KEYBOARD_MODIFIER_RIGHTSHIFT 0x20
#define KEYBOARD_MODIFIER_RIGHTALT 0x40
#define KEYBOARD_MODIFIER_RIGHTGUI 0x80

#define KEYBOARD_LED_NUMLOCK 0x01
#define KEYBOARD_LED_CAPSLOCK 0x02
#define KEYBOARD_LED_SCROLLLOCK 0x04

#endif /* _CORE_HID_USAGE_H_ */
