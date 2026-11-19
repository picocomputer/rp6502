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

/* The renderer an attribute names, or NULL for none. A fabric build has no
 * renderers. */
#ifndef RP6502_VGA_FABRIC
vga_fill_fn_t mode3_fill_fn(uint16_t attributes);
#endif

#endif /* _CORE_VGA_MODE3_H_ */
