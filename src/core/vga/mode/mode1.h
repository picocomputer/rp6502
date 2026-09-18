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

/* mode1_fill_fn returns the renderer an attribute names, and mode1_fill_attr
 * finds the attribute that names a renderer. A savestate stores the attribute
 * rather than the function address, because the function can be at a different
 * address in the build that loads it. mode1_fill_fn returns NULL in a fabric
 * build, and mode1_fill_attr is not compiled there. */
vga_fill_fn_t mode1_fill_fn(uint16_t attributes);
#ifndef RP6502_VGA_FABRIC
bool mode1_fill_attr(vga_fill_fn_t fn, uint16_t *attributes);
#endif

#endif /* _CORE_VGA_MODE1_H_ */
