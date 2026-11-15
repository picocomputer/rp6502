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

/* The keyboard driver of a machine that has no system under it: it types
 * through a layout and owns the locks, which start with NumLock on. */
void keymap_init(void);

// Auto-repeat. The held key is read back from keyboard.c, so a release ends
// it, and so does the terminal turning repeat off (DECARM).
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

/* Type through this layout of the database from the next key on. */
void keymap_use_layout(int index);

/* After KEYBOARD_DRIVER, whose start clears the locks. */
#define KEYMAP_DRIVER DRIVER(     \
    INIT(keymap_init)             \
    TASK_PUMPED(keymap_task))

#endif /* _CORE_HID_KEYMAP_H_ */
