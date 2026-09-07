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

/* Whether this mode has that attribute at all. What a booking asks, and the
 * only half of the list a machine whose fabric rasterizes has. */
bool mode1_fill_valid(uint16_t attributes);

/* The renderer an attribute names, and the attribute a renderer came from.
 * A savestate carries the attribute, never the address, so these are the two
 * directions it needs. NULL and absent respectively where the fabric draws. */
vga_fill_fn_t mode1_fill_fn(uint16_t attributes);
#ifndef RP6502_VGA_FABRIC
bool mode1_fill_attr(vga_fill_fn_t fn, uint16_t *attributes);
#endif

#endif /* _CORE_VGA_MODE1_H_ */
