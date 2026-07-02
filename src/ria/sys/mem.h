/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_MEM_H_
#define _RIA_SYS_MEM_H_

/* Various large chunks of memory used globally. This header is the
 * contract shared with the emulator (which mirrors it over its own
 * storage); the mbuf transfer engine is in sys/mem_hw.h.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// 64KB Extended RAM
// One page is tracked for audio
extern uint8_t *const xram;
extern volatile uint8_t xram_queue_page;
extern volatile uint8_t xram_queue_head;
extern volatile uint8_t xram_queue_tail;
extern volatile uint8_t xram_queue[256][2];

// The xstack is:
// 512 bytes, enough to hold a CC65 stack frame, two strings for a
// file rename, or a disk sector
// 1 byte at end+1 always zero for cstring and safety.
// Using xstack for cstrings doesn't require sending the zero termination.
#define XSTACK_SIZE 0x200
extern uint8_t xstack[];
extern volatile size_t xstack_ptr;

// RIA registers are located in uninitialized ram so they survive
// a soft reboot. A hard reboot with the physical button overwrites
// this memory which might be a security feature we can override.
extern volatile uint8_t regs[];
#define REGS(addr) regs[(addr) & 0x1F]
#define REGSW(addr) ((uint16_t *)&REGS(addr))[0]

#endif /* _RIA_SYS_MEM_H_ */
