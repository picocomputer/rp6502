/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_MODE2_H_
#define _CORE_VGA_MODE2_H_

#include "core/vga/prog.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool mode2_prog(uint16_t *xregs);

/* One renderer for all eight of this mode's classes: which class a row draws
 * is in mode2's own shadow rather than in the pointer, so the reverse needs
 * to be told which row it is asking about. */
vga_fill_fn_t mode2_fill_fn(uint16_t attributes);
bool mode2_fill_attr(int16_t scanline, int16_t plane, uint16_t *attributes);
void mode2_set_options(int16_t scanline, int16_t plane, uint16_t options);

#endif /* _CORE_VGA_MODE2_H_ */
