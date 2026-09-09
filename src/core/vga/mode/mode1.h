/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_MODE1_H_
#define _CORE_VGA_MODE1_H_

#include "core/vga/prog.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool mode1_prog(uint16_t *xregs);

bool mode1_fill_valid(uint16_t attributes);

/* The renderer an attribute names, and the attribute a renderer came from. A
 * savestate stores the attribute rather than the function address, because the
 * address belongs to the build that saved it. mode1_fill_fn returns NULL in a
 * fabric build and the reverse is not compiled. */
vga_fill_fn_t mode1_fill_fn(uint16_t attributes);
#ifndef RP6502_VGA_FABRIC
bool mode1_fill_attr(vga_fill_fn_t fn, uint16_t *attributes);
#endif

#endif /* _CORE_VGA_MODE1_H_ */
