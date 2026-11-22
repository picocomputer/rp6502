/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_LAYOUT_H_
#define _CORE_HID_LAYOUT_H_

/* The layouts: the database, generated from src/core/def/keyboard_*.def by
 * src/core/hid/layout.py, and the user's list of them, the SET KB setting.
 * A layout is named by its index, which is below layout_count().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Columns of layout_code_point, by the modifiers that select them.
#define LAYOUT_PLAIN 0
#define LAYOUT_SHIFT 1
#define LAYOUT_ALTGR 2
#define LAYOUT_SHIFT_ALTGR 3

int layout_count(void);
const char *layout_name(int idx);
const char *layout_description(int idx);

// Keycodes are HID, 0-127. Layouts carry nothing above the keypad.
uint16_t layout_code_point(int idx, uint8_t keycode, unsigned col);
bool layout_use_caps(int idx, uint8_t keycode);

// Dead keys, as code points. A dead2 entry is {dead, base, result} and a
// dead3 entry is {dead, dead, base, result}.
unsigned layout_dead2_count(int idx);
unsigned layout_dead3_count(int idx);
uint16_t layout_dead2(int idx, unsigned entry, unsigned field);
uint16_t layout_dead3(int idx, unsigned entry, unsigned field);

/* The user's list: layout names with spaces between, typed through one at a
 * time. A machine with no stored list adopts the default layout. */
#define LAYOUT_LIST_SIZE 40
void layout_list_init(void);

// Types through the next layout of the list, going around at the end.
void layout_list_cycle(void);

/* The setting's check, apply and monitor answers. The check writes the list
 * with single spaces and the database's spelling of each name, and refuses an
 * unknown or repeated layout, an empty list, and one too long to store. The
 * help answer lists every layout in the database, a line per call. */
bool layout_check_list(const char *in, char *out);
void layout_apply_list(const char *list, bool changed);
int layout_list_response(char *buf, size_t buf_size, int state, unsigned width);
int layout_help_response(char *buf, size_t buf_size, int state, unsigned width);

#define LAYOUT_DRIVER DRIVER(                                              \
    INIT(layout_list_init)                                                 \
    CONFIG_RAW(L, layout, list, LAYOUT_LIST_SIZE, "",                      \
               layout_check_list, layout_apply_list,                       \
               STR_KB, layout_list_response, STR_HELP_SET_KB, layout_help_response))

#endif /* _CORE_HID_LAYOUT_H_ */
