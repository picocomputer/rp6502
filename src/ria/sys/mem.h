/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MEM_H_
#define _MEM_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// 64KB Extended RAM
#ifdef NDEBUG
extern uint8_t xram[0x10000];
#else
extern uint8_t *const xram;
#endif

// The xstack is:
// 256 bytes, enough to hold a CC65 stack frame.
// 1 byte at end+1 always zero for cstring and safety.
// Using xstack fort cstrings doesn't require sending the zero termination.
#define XSTACK_SIZE 0x100
extern uint8_t xstack[];
extern volatile size_t xstack_ptr;

// RIA registers are located at the bottom of cpu1 stack.
// cpu1 runs the action loop and uses very little stack.
extern uint8_t regs[0x20];
#define REGS(addr) regs[(addr) & 0x1F]
#define REGSW(addr) ((uint16_t *)&REGS(addr))[0]
asm(".equ regs, 0x20040000");

// Misc memory buffer for moving things around.
// 6502 <-> RAM, USB <-> RAM, UART <-> RAM, etc.
#define MBUF_SIZE 1024
extern uint8_t mbuf[MBUF_SIZE];
extern size_t mbuf_len;

#endif /* _MEM_H_ */
