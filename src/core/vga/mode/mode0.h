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

vga_fill_fn_t mode0_fill_fn(uint16_t attributes);
bool mode0_fill_attr(vga_fill_fn_t fn, uint16_t *attributes);

/* The scanline the terminal's program begins at. It cannot be recovered from
 * the scanline table afterwards, because a graphics mode programmed later can
 * overwrite the lowest-numbered rows the terminal installed. */
int16_t mode0_begin(void);
void mode0_set_begin(int16_t at);

#endif /* _CORE_VGA_MODE0_H_ */
