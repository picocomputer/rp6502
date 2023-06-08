
/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _XRAM_H_
#define _XRAM_H_

#include <stdint.h>

#define XRAM_RW0 REGS(0xFFE4)
#define XRAM_STEP0 *(int8_t *)&REGS(0xFFE5)
#define XRAM_ADDR0 REGSW(0xFFE6)

#define XRAM_RW1 REGS(0xFFE8)
#define XRAM_STEP1 *(int8_t *)&REGS(0xFFE9)
#define XRAM_ADDR1 REGSW(0xFFEA)

// 64KB Extended RAM
#ifdef NDEBUG
extern uint8_t xram[0x10000];
#else
extern uint8_t *const xram;
#endif

#endif /* _XRAM_H_ */
