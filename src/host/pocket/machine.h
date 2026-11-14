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
#define HOST_REGISTER_RAM(name) name

/* APF has four controller slots, and apf.c mounts at most one HID slot
 * for each of them. */
#define HID_MAX_SLOTS 4
/* A program that ends returns the machine to its monitor. */
#define RP6502_HAS_MONITOR 0
#define TERM_MAX_HEIGHT 30
/* Terminal replies are the only bytes pushed into the console rings on
 * this machine. core/com/manifold.c queues at most 32 bytes of them, so a
 * ring of 32 takes a full one. */
#define MANIFOLD_RING_SIZE 32
/* The C library's stdout is the console, so com_printf formats straight to
 * it. */
#define COM_LIBC_STDOUT 1


#endif /* _HOST_MACHINE_H_ */
