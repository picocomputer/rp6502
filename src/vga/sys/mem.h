/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MEM_H_
#define _MEM_H_

#include <stdint.h>

#define XRAM_RW0 REGS(0xFFE4)
#define XRAM_STEP0 *(int8_t *)&REGS(0xFFE5)
#define XRAM_ADDR0 REGSW(0xFFE6)

#define XRAM_RW1 REGS(0xFFE8)
#define XRAM_STEP1 *(int8_t *)&REGS(0xFFE9)
#define XRAM_ADDR1 REGSW(0xFFEA)

// 64KB Extended RAM
extern volatile const uint8_t xram[0x10000];
asm(".equ xram, 0x20030000");

#endif /* _MEM_H_ */
