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
/* The RIA registers, in core 1's stack bank outside the striped SRAM. */
#define HOST_REGISTER_RAM(name) __scratch_x(#name) name

#define XRAM_ALIGN 4
#define HID_MAX_SLOTS 16
/* A program that ends returns the machine to its monitor. */
#define RP6502_HAS_MONITOR 1


#endif /* _HOST_MACHINE_H_ */
