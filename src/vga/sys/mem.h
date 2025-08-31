/*
 * Copyright (c) 2025 Rumbledethumps
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
#ifdef NDEBUG
extern volatile const uint8_t xram[0x10000];
#else
extern volatile uint8_t *const xram;
#endif

#endif /* _VGA_SYS_MEM_H_ */
