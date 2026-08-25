/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The memory the 6502 can see: extended RAM, the extended stack, and the RIA's
 * register window. Each machine supplies the storage -- SRAM on a Pico, an
 * array in the emulator, an SDRAM window on a Pocket. */

#ifndef _CORE_MEM_H_
#define _CORE_MEM_H_

#include <stddef.h>
#include <stdint.h>

/* 64KB Extended RAM. Volatile because something else writes it while the
 * machine reads: the 6502 through the RIA on a Pico, DMA off the PIX bus on the
 * VGA, the fabric on a Pocket. One page is tracked for audio. */
extern volatile uint8_t *const xram;
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
#define REGSL(addr) ((uint32_t *)&REGS(addr))[0]

// CRC-32/ISO-HDLC (zlib). mem_crc32(0, buf, len) is the one-shot value; chain by
// feeding a prior result back as crc.
uint32_t mem_crc32(uint32_t crc, const void *buf, size_t len);

#endif /* _CORE_MEM_H_ */
