/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_LAYOUT_H_
#define _CORE_HID_LAYOUT_H_

/* The layouts: the database, generated from src/core/def/keyboard_*.def by
 * src/core/hid/layout.py, and the user's list of them, the SET KB setting.
 *
 * Every read goes through layout_word, which the platform supplies: a RIA
 * links the tables into flash, and the Pocket stages them in SDRAM and fetches
 * one word at a time through a window that cannot fetch a halfword. Packed
 * this way the whole database is eight kilobytes, which is what lets a machine
 * with no flash carry all of it.
 *
 * Names and descriptions are copied out rather than pointed at, because a
 * staging window is not memory a char pointer can address.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Buffer sizes, NUL included. The generator refuses anything longer.
#define LAYOUT_NAME_MAX 8
#define LAYOUT_DESC_MAX 32

// Columns of layout_code_point, by the modifiers that select them.
#define LAYOUT_PLAIN 0
#define LAYOUT_SHIFT 1
#define LAYOUT_ALTGR 2
#define LAYOUT_SHIFT_ALTGR 3

// False when no database arrived. Everything below then reads empty, which
// types nothing and leaves the control keys working.
bool layout_init(void);

int layout_count(void);
void layout_name(int idx, char *buf);        // LAYOUT_NAME_MAX bytes
void layout_description(int idx, char *buf); // LAYOUT_DESC_MAX bytes

// Keycodes are HID, 0-127. Layouts carry nothing above the keypad.
uint16_t layout_code_point(int idx, uint8_t keycode, unsigned col);
bool layout_use_caps(int idx, uint8_t keycode);

// Dead keys, as code points. A dead2 entry is {dead, base, result} and a
// dead3 entry is {dead, dead, base, result}.
unsigned layout_dead2_count(int idx);
unsigned layout_dead3_count(int idx);
uint16_t layout_dead2(int idx, unsigned entry, unsigned field);
uint16_t layout_dead3(int idx, unsigned entry, unsigned field);

// One 16-bit word of the database image, by index. Each platform writes its
// own.
uint16_t layout_word(uint32_t index);

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
    INIT(layout_init)                                                      \
    INIT(layout_list_init)                                                 \
    CONFIG_RAW(L, layout, list, LAYOUT_LIST_SIZE, "",                      \
               layout_check_list, layout_apply_list,                       \
               STR_KB, layout_list_response, STR_HELP_SET_KB, layout_help_response))

#endif /* _CORE_HID_LAYOUT_H_ */
