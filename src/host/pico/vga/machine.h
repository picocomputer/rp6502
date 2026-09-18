/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_MACHINE_H_
#define _HOST_MACHINE_H_

#include <pico.h>

#define HOST_IN_FLASH(group) __in_flash(group)
#define HOST_NOT_IN_FLASH(group) __not_in_flash(group)
#define HOST_UNINITIALIZED_RAM(name) __uninitialized_ram(name)

/* The PIX DMA chain in pix.c writes each 16-bit xram address into the low
 * half of a pointer to xram, so xram must sit on a 64 KB boundary, which no
 * other machine needs. */
#define XRAM_ALIGN 0x10000

/* The SXGA console has 512 scanlines and a 16-line font, which gives 32
 * rows. */
#define TERM_MAX_HEIGHT 32


#endif /* _HOST_MACHINE_H_ */
