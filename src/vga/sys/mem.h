/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_MEM_H_
#define _VGA_SYS_MEM_H_

/* Storage for XRAM
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// 64KB Extended RAM
extern volatile uint8_t *const xram;

/* Every XRAM base an xreg carries — a mode's config struct, a sprite
 * descriptor array, the PSG's channel block — must be 32-bit aligned.
 * False means unusable; what a host makes of that is its own business
 * and they differ. */
bool mem_xram_align(uint16_t addr);

#endif /* _VGA_SYS_MEM_H_ */
