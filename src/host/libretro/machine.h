/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_MACHINE_H_
#define _HOST_MACHINE_H_

#define HOST_IN_FLASH
#define HOST_IN_FLASH_FUNC
#define HOST_IN_RAM
#define HOST_IN_RAM_FUNC
#define HOST_ISR_FUNC
#define HOST_UNINITIALIZED_RAM(name) name
#define HOST_REGISTER_RAM(name) name

#define XRAM_ALIGN 4
#define HID_MAX_SLOTS 16
/* A program that ends returns the machine to its monitor. */
#define RP6502_HAS_MONITOR 0
#define TERM_MAX_HEIGHT 30
#define MANIFOLD_RING_SIZE 256
/* The C library's stdout is the host's, so com_printf formats into a buffer
 * of its own. */
#define COM_LIBC_STDOUT 0


#endif /* _HOST_MACHINE_H_ */
