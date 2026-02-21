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
// One page is tracked for audio
extern uint8_t *const xram;
extern volatile uint8_t xram_queue_page;
extern volatile uint8_t xram_queue_head;
extern volatile uint8_t xram_queue_tail;
extern uint8_t xram_queue[256][2];

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

// Misc memory buffer for moving things around.
// 6502 <-> RAM, USB <-> RAM, UART <-> RAM, etc.
// Also used as a littlefs buffer for read/write.
#define MBUF_SIZE 1024
extern uint8_t mbuf[];
extern size_t mbuf_len;

// Memory buffer reading
typedef void (*mem_read_callback_t)(bool timeout);
void mem_task(void);
void mem_break(void);
void mem_read_mbuf(uint32_t timeout_ms, mem_read_callback_t callback, size_t size);

#endif /* _RIA_SYS_MEM_H_ */
