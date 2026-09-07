/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_MODE5_H_
#define _CORE_VGA_MODE5_H_

#include "core/vga/prog.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool mode5_prog(uint16_t *xregs);

/* The renderer an attribute names, and the attribute a renderer came from.
 * A savestate carries the attribute, never the address. */
vga_sprite_fn_t mode5_sprite_fn(uint16_t attributes);
bool mode5_sprite_attr(vga_sprite_fn_t fn, uint16_t *attributes);

#endif /* _CORE_VGA_MODE5_H_ */
