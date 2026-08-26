/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The keyboard's terminal half: keys in, console characters out.
 *
 * kbd.c reports which keys are down; this decides what that spells. A host
 * whose OS already produced characters -- a desktop, where the layout, the
 * dead keys and the code page are the window system's job -- links a null
 * implementation and never compiles a layout.
 */

#ifndef _CORE_HID_KBT_H_
#define _CORE_HID_KBT_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void kbt_init(void);

// Auto-repeat: the held key is re-read from kbd, so a release ends it.
void kbt_task(void);

// One key pressed, by value. Repeats come from kbt_task, not from here.
void kbt_key_down(uint8_t modifier, uint8_t keycode);

// The modifier byte after every report, for the Alt-code release.
void kbt_modifiers(uint8_t modifier);

// Drain the character queue into buf.
size_t kbt_in_chars(char *buf, size_t length);

// Responder prints all keyboard layout options.
int kbt_layouts_response(char *buf, size_t buf_size, int state, unsigned width);


// Configuration setting KB
#define KBT_LAYOUT_LIST_SIZE 40
void kbt_load_layout(const char *str);
bool kbt_set_layout(const char *list);
const char *kbt_get_layout_list(void);
const char *kbt_get_layout(void);
const char *kbt_get_layout_verbose(void);

#endif /* _CORE_HID_KBT_H_ */
