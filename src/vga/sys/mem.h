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

#endif /* _VGA_SYS_MEM_H_ */
