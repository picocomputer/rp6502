/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_MACHINE_H_
#define _HOST_MACHINE_H_

#include <pico.h>

/* Code and data take separate sections, because GCC rejects both in one
 * named section of a file. A function the compiler inlines lands where its
 * caller is, so HOST_IN_FLASH_FUNC is never inlined. */
#define HOST_IN_FLASH __in_flash("data")
#define HOST_IN_FLASH_FUNC __noinline __in_flash("code")
#define HOST_IN_RAM __not_in_flash("data")
#define HOST_IN_RAM_FUNC __not_in_flash("code")
/* An interrupt handler runs from RAM with everything it calls inlined. */
#define HOST_ISR_FUNC __attribute__((flatten, optimize("O3"))) __not_in_flash("code")
#define HOST_UNINITIALIZED_RAM(name) __uninitialized_ram(name)

/* The PIX DMA chain in pix.c writes each 16-bit xram address into the low
 * half of a pointer to xram, so xram must sit on a 64 KB boundary, which no
 * other machine needs. */
#define XRAM_ALIGN 0x10000

/* The SXGA console has 512 scanlines and a 16-line font, which gives 32
 * rows. */
#define TERM_MAX_HEIGHT 32


#endif /* _HOST_MACHINE_H_ */
