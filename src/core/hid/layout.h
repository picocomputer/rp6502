/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_LAYOUT_H_
#define _CORE_HID_LAYOUT_H_

/* The keyboard layout database, generated from def/keyboard_*.def by
 * src/core/gen/keyboard_layout_gen.py.
 *
 * Every read goes through layout_word, which the platform supplies: a RIA
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
#define LAYOUT_NAME_MAX 8
#define LAYOUT_DESC_MAX 32

// Columns of layout_code_point, by the modifiers that select them.
#define LAYOUT_PLAIN 0
#define LAYOUT_SHIFT 1
#define LAYOUT_ALTGR 2
#define LAYOUT_SHIFT_ALTGR 3

// False when no database arrived. Everything below then reads empty,
// which types nothing and leaves the control keys working.
bool layout_init(void);

int layout_count(void);
void layout_name(int idx, char *buf);        // LAYOUT_NAME_MAX bytes
void layout_description(int idx, char *buf); // LAYOUT_DESC_MAX bytes

// Keycodes are HID, 0-127. Layouts carry nothing above the keypad.
uint16_t layout_code_point(int idx, uint8_t keycode, unsigned col);
bool layout_use_caps(int idx, uint8_t keycode);

// Dead keys, as code points. dead2 is {dead, base, result} and dead3 is
// {dead, dead, base, result}.
unsigned layout_dead2_count(int idx);
unsigned layout_dead3_count(int idx);
uint16_t layout_dead2(int idx, unsigned entry, unsigned field);
uint16_t layout_dead3(int idx, unsigned entry, unsigned field);

// One 16-bit word of the database image, by index. Written per platform.
uint16_t layout_word(uint32_t index);

/* This driver's row in a machine's driver list; see core/sys/driver.h. layout_init returns
 * whether a database arrived; a driver walk does not ask. */
#define LAYOUT_DRIVER DRIVER(layout_init, nul_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _CORE_HID_LAYOUT_H_ */
