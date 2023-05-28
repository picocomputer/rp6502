/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MEM_H_
#define _MEM_H_

#include <stddef.h>
#include <stdint.h>

// Registers are located at the bottom of cpu1 stack.
// cpu1 runs the ria action loop and uses very little stack.
extern uint8_t regs[0x20];
#define REGS(addr) regs[(addr)&0x1F]
#define REGSW(addr) ((uint16_t *)&REGS(addr))[0]
asm(".equ regs, 0x20040000");

// Buffer for moving things around.
// 6502 <-> RAM, USB <-> RAM, UART <-> RAM, etc.
#define MBUF_SIZE 1024
extern uint8_t mbuf[MBUF_SIZE];
extern size_t mbuf_len;
uint32_t mbuf_crc32();

#define XSTACK_SIZE 0x100
extern uint8_t xstack[];
extern volatile size_t xstack_ptr;

// 64KB Extended RAM
#ifdef NDEBUG
extern uint8_t xram[0x10000];
#else
extern uint8_t *const xram;
#endif

#define XRAM_RW0 REGS(0xFFE4)
#define XRAM_STEP0 *(int8_t *)&REGS(0xFFE5)
#define XRAM_ADDR0 REGSW(0xFFE6)

#define XRAM_RW1 REGS(0xFFE8)
#define XRAM_STEP1 *(int8_t *)&REGS(0xFFE9)
#define XRAM_ADDR1 REGSW(0xFFEA)

#endif /* _MEM_H_ */
