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

/* The renderer an attribute names, and the attribute a renderer came from.
 * A savestate carries the attribute, never the address. */
vga_fill_fn_t mode1_fill_fn(uint16_t attributes);
bool mode1_fill_attr(vga_fill_fn_t fn, uint16_t *attributes);

#endif /* _CORE_VGA_MODE1_H_ */
