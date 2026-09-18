/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_MODE3_H_
#define _CORE_VGA_MODE3_H_

#include "core/vga/prog.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool mode3_prog(uint16_t *xregs);

bool mode3_fill_valid(uint16_t attributes);

/* mode3_fill_fn returns the renderer an attribute names, and mode3_fill_attr
 * finds the attribute that names a renderer. A savestate stores the attribute
 * rather than the function address, because the function can be at a different
 * address in the build that loads it. mode3_fill_fn returns NULL in a fabric
 * build, and mode3_fill_attr is not compiled there. */
vga_fill_fn_t mode3_fill_fn(uint16_t attributes);
#ifndef RP6502_VGA_FABRIC
bool mode3_fill_attr(vga_fill_fn_t fn, uint16_t *attributes);
#endif

#endif /* _CORE_VGA_MODE3_H_ */
