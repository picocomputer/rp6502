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

/* Whether this mode has that attribute at all. What a booking asks, and the
 * only half of the list a machine whose fabric rasterizes has. */
bool mode4_sprite_valid(uint16_t attributes);

/* The renderer an attribute names, and the attribute a renderer came from.
 * A savestate carries the attribute, never the address, so these are the two
 * directions it needs. NULL and absent respectively where the fabric draws. */
vga_sprite_fn_t mode4_sprite_fn(uint16_t attributes);
#ifndef RP6502_VGA_FABRIC
bool mode4_sprite_attr(vga_sprite_fn_t fn, uint16_t *attributes);
#endif

#endif /* _CORE_VGA_MODE4_H_ */
