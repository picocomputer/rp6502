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

/* One renderer draws every mode 2 attribute, so the attribute a row was
 * programmed with is kept in mode2.c per scanline and plane rather than in the
 * function pointer, and the reverse lookup a savestate needs has to name a row
 * instead of a renderer. */
vga_fill_fn_t mode2_fill_fn(uint16_t attributes);
bool mode2_fill_attr(int16_t scanline, int16_t plane, uint16_t *attributes);
void mode2_set_options(int16_t scanline, int16_t plane, uint16_t options);

#endif /* _CORE_VGA_MODE2_H_ */
