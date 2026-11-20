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

/* The config at config_ptr is on a word and inside xram. */
bool mode1_config_ok(uint16_t config_ptr);

bool mode1_fill_valid(uint16_t attributes);

/* The renderer an attribute names, or NULL for none. A fabric build has no
 * renderers. */
#ifndef RP6502_VGA_FABRIC
vga_fill_fn_t mode1_fill_fn(uint16_t attributes);
#endif

#endif /* _CORE_VGA_MODE1_H_ */
