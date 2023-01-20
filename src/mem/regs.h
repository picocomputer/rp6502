/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _REGS_H_
#define _REGS_H_

#include <stdint.h>

// Registers are located at the bottom of cpu1 stack.
// cpu1 runs the ria action loop and uses very little stack.
extern uint8_t regs[0x20];
#define REGS(addr) regs[(addr) & 0x1F]
#define REGSW(addr) ((uint16_t *)&REGS(addr))[0]
asm(".equ regs, 0x20040000");

#endif /* _REGS_H_ */
