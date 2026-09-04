/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_RIA_REGS_H_
#define _CORE_RIA_REGS_H_

/* The RIA's register window, the extended stack behind it, and the
 * write-notify queue its RW engine feeds. The C twin of regs.sv: a machine
 * with the RIA in fabric has the first two there and no queue at all. */

#include <stddef.h>
#include <stdint.h>

// RIA registers are located in uninitialized ram so they survive
// a soft reboot. A hard reboot with the physical button overwrites
// this memory which might be a security feature we can override.
extern volatile uint8_t regs[];
#define REGS(addr) regs[(addr) & 0x1F]
#define REGSW(addr) ((uint16_t *)&REGS(addr))[0]
#define REGSL(addr) ((uint32_t *)&REGS(addr))[0]

// The xstack is:
// 512 bytes, enough to hold a CC65 stack frame, two strings for a
// file rename, or a disk sector
// 1 byte at end+1 always zero for cstring and safety.
// Using xstack for cstrings doesn't require sending the zero termination.
#define XSTACK_SIZE 0x200
extern uint8_t xstack[];
extern volatile size_t xstack_ptr;

/* One page of xram is tracked for audio: every write the RW engine lands on
 * it is queued as (low byte, value) for the device's sampler to drain. */
extern volatile uint8_t xram_queue_page;
extern volatile uint8_t xram_queue_head;
extern volatile uint8_t xram_queue_tail;
extern volatile uint8_t xram_queue[256][2];

#endif /* _CORE_RIA_REGS_H_ */
