
/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VRAM_H_
#define _VRAM_H_

#include <stdint.h>

#define VRAM_RW0 REGS(0xFFE4)
#define VRAM_STEP0 (int8_t) REGS(0xFFE5)
#define VRAM_ADDR0 REGSW(0xFFE6)

#define VRAM_RW1 REGS(0xFFE8)
#define VRAM_STEP1 (int8_t) REGS(0xFFE9)
#define VRAM_ADDR1 REGSW(0xFFEA)

extern volatile uint16_t vram_ptr0;
extern volatile uint16_t vram_ptr1;

// 64KB Virtual RAM
#ifdef NDEBUG
extern uint8_t vram[0x10000];
#else
extern uint8_t *const vram;
#endif

#endif /* _VRAM_H_ */
