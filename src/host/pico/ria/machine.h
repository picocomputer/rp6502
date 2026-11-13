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
/* The RIA registers, in core 1's stack bank outside the striped SRAM. The
 * section is NOLOAD, so they keep their values through a reboot; see
 * pico/ld/section_extra_post_scratch.incl. */
#define HOST_REGISTER_RAM(name) __attribute__((section(".scratch_x_noload." #name))) name

#define HID_MAX_SLOTS 16
/* A program that ends returns the machine to its monitor. */
#define RP6502_HAS_MONITOR 1
/* The C library's stdout is the console, so com_printf formats straight to
 * it. */
#define COM_LIBC_STDOUT 1
/* The size of the UART's input ring. */
#define MANIFOLD_RING_SIZE 32


#endif /* _HOST_MACHINE_H_ */
