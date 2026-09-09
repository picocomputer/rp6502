/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_RIA_REGS_H_
#define _CORE_RIA_REGS_H_

#include <stddef.h>
#include <stdint.h>

extern volatile uint8_t regs[];
#define REGS(addr) regs[(addr) & 0x1F]
#define REGSW(addr) ((uint16_t *)&REGS(addr))[0]
#define REGSL(addr) ((uint32_t *)&REGS(addr))[0]

/* 512 bytes holds a CC65 stack frame, the two names of a file rename, or a
 * disk sector. No push reaches the byte at XSTACK_SIZE, so it holds the zero
 * that terminates a string pushed all the way to the top of the stack; a
 * caller pushing a C string therefore need not push its terminator. */
#define XSTACK_SIZE 0x200
extern uint8_t xstack[];
extern volatile size_t xstack_ptr;

/* An audio device names one page of XRAM to watch. A write that the RW engine
 * lands on that page is queued as (low byte, value) for the device to drain
 * while it samples; a write arriving on a full queue lands in XRAM
 * unreported. */
extern volatile uint8_t xram_queue_page;
extern volatile uint8_t xram_queue_head;
extern volatile uint8_t xram_queue_tail;
extern volatile uint8_t xram_queue[256][2];

#endif /* _CORE_RIA_REGS_H_ */
