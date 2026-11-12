/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_KEYMAP_H_
#define _CORE_HID_KEYMAP_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* keyboard.c calls these on every new key press and after every report. This
 * file answers them with a layout, dead keys and a code page. A machine whose
 * host resolved the characters before the keystroke arrived links
 * core/hid/vtkeys.c instead, which answers with nothing. */
void keymap_on_key(uint8_t modifier, uint8_t keycode);
void keymap_on_modifiers(uint8_t modifier);

void keymap_init(void);

// Auto-repeat. The held key is read back from keyboard.c, so a release ends it.
void keymap_task(void);

/* The queue of typed bytes, which holds KEYMAP_KEY_QUEUE_SIZE of them. */
#define KEYMAP_KEY_QUEUE_SIZE 16
size_t keymap_in_chars(char *buf, size_t length);
int keymap_in_peek(size_t offset);

/* Drop whatever was half-typed: the queue, and any dead key or Alt code still
 * being composed. */
void keymap_abandon(void);

/* The keyboard row core/com/com.c reads on a machine with a layout engine of
 * its own. There is no dwell, because a pause here is a person who stopped
 * typing rather than a gap in a burst arriving over a wire. */
#define KEYMAP_COM_SOURCE {.read = keymap_in_chars, .peek = keymap_in_peek, \
    .clear = keymap_abandon, .size = KEYMAP_KEY_QUEUE_SIZE}

int keymap_layouts_response(char *buf, size_t buf_size, int state, unsigned width);

#define KEYMAP_LAYOUT_LIST_SIZE 40
bool keymap_check_layout_list(const char *in, char *out);
void keymap_apply_layout_list(const char *list, bool changed);
int keymap_layout_list_response(char *buf, size_t buf_size, int state, unsigned width);
const char *keymap_get_layout(void);
const char *keymap_get_layout_verbose(void);

#define KEYMAP_CONFIG_LAYOUT_LIST CONFIG_RAW(L, keymap, layout_list, \
    KEYMAP_LAYOUT_LIST_SIZE, "", keymap_check_layout_list, keymap_apply_layout_list, \
    STR_KB, keymap_layout_list_response, STR_HELP_SET_KB, keymap_layouts_response)
#define KEYMAP_DRIVER DRIVER(  \
    INIT(keymap_init)          \
    TASK_PUMPED(keymap_task)   \
    KEYMAP_CONFIG_LAYOUT_LIST)

#endif /* _CORE_HID_KEYMAP_H_ */
