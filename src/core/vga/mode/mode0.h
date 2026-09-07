/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_MODE0_H_
#define _CORE_VGA_MODE0_H_

#include "core/vga/prog.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool mode0_prog(uint16_t *xregs);

/* One renderer, so the attribute is always zero. The pair exists so the
 * dispatch above can ask every mode the same question. */
vga_fill_fn_t mode0_fill_fn(uint16_t attributes);
bool mode0_fill_attr(vga_fill_fn_t fn, uint16_t *attributes);

/* Where the terminal's own program starts, which is not derivable from the
 * table: a later booking may overwrite the lowest rows it installed. */
int16_t mode0_begin(void);
void mode0_set_begin(int16_t at);

#endif /* _CORE_VGA_MODE0_H_ */
