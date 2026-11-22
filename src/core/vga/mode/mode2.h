/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_MODE2_H_
#define _CORE_VGA_MODE2_H_

#include "core/vga/prog.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool mode2_prog(uint16_t *xregs);

/* The config at config_ptr is on a word and inside xram. */
bool mode2_config_ok(uint16_t config_ptr);

/* One renderer draws every mode 2 attribute, so the attribute a row was
 * programmed with is kept in mode2.c per scanline and plane rather than in the
 * function pointer, and the reverse lookup a savestate needs has to name a row
 * instead of a renderer. */
bool mode2_fill_valid(uint16_t options);

/* The renderer, which is handed the options, or NULL for options that are
 * not valid. A fabric build has no renderers. */
#ifndef RP6502_VGA_FABRIC
vga_fill_fn_t mode2_fill_fn(uint16_t options);
#endif

#endif /* _CORE_VGA_MODE2_H_ */
