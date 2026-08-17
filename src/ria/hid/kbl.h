/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_HID_KBL_H_
#define _RIA_HID_KBL_H_

/* The keyboard layout database, generated from def/kbd_*.def by
 * src/gen/kbd_layout_gen.py.
 *
 * Every read goes through kbl_word, which the platform supplies: a RIA
 * links the tables into flash, the Pocket stages them in SDRAM and
 * fetches a word at a time through a window that cannot do halfwords.
 * A layout is twenty kilobytes of table the way a compiler lays it out
 * and eight the way this does, which is the difference between a
 * machine that can carry all of them and one that cannot.
 *
 * Names and descriptions are copied out rather than pointed at, because
 * a staging window is not memory a char pointer can address.
 */

#include <stdbool.h>
#include <stdint.h>

// Buffer sizes, NUL included. The generator refuses anything longer.
#define KBL_NAME_MAX 8
#define KBL_DESC_MAX 32

// Columns of kbl_code_point, by the modifiers that select them.
#define KBL_PLAIN 0
#define KBL_SHIFT 1
#define KBL_ALTGR 2
#define KBL_SHIFT_ALTGR 3

// False when no database arrived. Everything below then reads empty,
// which types nothing and leaves the control keys working.
bool kbl_init(void);

int kbl_count(void);
void kbl_name(int layout, char *buf);        // KBL_NAME_MAX bytes
void kbl_description(int layout, char *buf); // KBL_DESC_MAX bytes

// Keycodes are HID, 0-127. Layouts carry nothing above the keypad.
uint16_t kbl_code_point(int layout, uint8_t keycode, unsigned col);
bool kbl_use_caps(int layout, uint8_t keycode);

// Dead keys, as code points. dead2 is {dead, base, result} and dead3 is
// {dead, dead, base, result}.
unsigned kbl_dead2_count(int layout);
unsigned kbl_dead3_count(int layout);
uint16_t kbl_dead2(int layout, unsigned entry, unsigned field);
uint16_t kbl_dead3(int layout, unsigned entry, unsigned field);

// One 16-bit word of the database image, by index. Written per platform.
uint16_t kbl_word(uint32_t index);

#endif /* _RIA_HID_KBL_H_ */
