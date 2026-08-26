/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The keyboard's terminal half: keys in, console characters out.
 *
 * keyboard.c reports which keys are down and offers each press to whatever spells
 * for this machine -- core/hid/keyboard.h declares that seam. This answers it with
 * a layout, dead keys and a code page. A machine whose host produced the
 * characters first answers the same seam with nothing and takes text instead,
 * which is a different file and not this contract.
 */

#ifndef _CORE_HID_KEYMAP_H_
#define _CORE_HID_KEYMAP_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void keymap_init(void);

// Auto-repeat: the held key is re-read from keyboard, so a release ends it.
void keymap_task(void);

// Drain the character queue into buf.
size_t keymap_in_chars(char *buf, size_t length);

// Responder prints all keyboard layout options.
int keymap_layouts_response(char *buf, size_t buf_size, int state, unsigned width);


// Configuration setting KB
#define KEYMAP_LAYOUT_LIST_SIZE 40
void keymap_load_layout(const char *str);
bool keymap_set_layout(const char *list);
const char *keymap_get_layout_list(void);
const char *keymap_get_layout(void);
const char *keymap_get_layout_verbose(void);

#endif /* _CORE_HID_KEYMAP_H_ */
