/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_MEM_H_
#define _RIA_SYS_MEM_H_

/* Various large chunks of memory used globally.
 */

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
// 512 bytes, enough to hold a CC65 stack frame, two strings for a
// file rename, or a disk sector
// 1 byte at end+1 always zero for cstring and safety.
// Using xstack for cstrings doesn't require sending the zero termination.
#define XSTACK_SIZE 0x200
extern uint8_t xstack[];
extern volatile size_t xstack_ptr;

// RIA registers are located at the bottom of cpu1 stack.
// cpu1 runs the action loop and uses very little stack.
// On the RP2040 these registers persist a press of the REBOOT
// button, but the RP2350 changes FFFC-FFFF for some reason.
extern volatile uint8_t regs[0x20];
#define REGS(addr) regs[(addr) & 0x1F]
#define REGSW(addr) ((uint16_t *)&REGS(addr))[0]
#if PICO_RP2040 == 1
asm(".equ regs, 0x20040000");
#elif PICO_RP2350 == 1
asm(".equ regs, 0x20080000");
#else
#error "Unknown microcontroller"
#endif

// Misc memory buffer for moving things around.
// 6502 <-> RAM, USB <-> RAM, UART <-> RAM, etc.
// Also used as a littlefs buffer for save/load.
#define MBUF_SIZE 1024
extern uint8_t mbuf[];
extern size_t mbuf_len;

#endif /* _RIA_SYS_MEM_H_ */
