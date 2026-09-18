/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_MODE4_H_
#define _CORE_VGA_MODE4_H_

#include "core/vga/prog.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool mode4_prog(uint16_t *xregs);

/* Whether this mode defines that attribute. mode4_prog asks this rather than
 * asking for the renderer, because a fabric build has no renderer to name. */
bool mode4_sprite_valid(uint16_t attributes);

/* mode4_sprite_fn returns the renderer an attribute names, and
 * mode4_sprite_attr finds the attribute that names a renderer. A savestate
 * stores the attribute rather than the function address, because the function
 * can be at a different address in the build that loads it. mode4_sprite_fn
 * returns NULL in a fabric build, and mode4_sprite_attr is not compiled
 * there. */
vga_sprite_fn_t mode4_sprite_fn(uint16_t attributes);
#ifndef RP6502_VGA_FABRIC
bool mode4_sprite_attr(vga_sprite_fn_t fn, uint16_t *attributes);
#endif

#endif /* _CORE_VGA_MODE4_H_ */
