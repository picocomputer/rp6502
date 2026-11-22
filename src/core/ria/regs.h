/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_RIA_REGS_H_
#define _CORE_RIA_REGS_H_

#include "core/sys/sst.h"
#include <stddef.h>
#include <stdint.h>

extern volatile uint8_t regs[];
#define REGS(addr) regs[(addr) & 0x1F]
#define REGSW(addr) ((uint16_t *)&REGS(addr))[0]
#define REGSL(addr) ((uint32_t *)&REGS(addr))[0]

/* The bits of READY, $FFE0. */
#define RIA_UART_RX_READY 0x40
#define RIA_UART_TX_READY 0x80

/* 512 bytes holds a CC65 stack frame, the two names of a file rename, or a
 * disk sector. No push reaches the byte at XSTACK_SIZE, so it holds the zero
 * that terminates a string pushed all the way to the top of the stack; a
 * caller pushing a C string therefore need not push its terminator. */
#define XSTACK_SIZE 0x200
extern uint8_t xstack[];
extern volatile uint16_t xstack_ptr;

/* After a failed savestate load: puts back the empty xstack and the zero
 * above its top that string pops end on. */
void regs_reset(void);

extern const sst_table_t regs_sst;

/* The register file, the xstack and its pointer. */
#define REGS_DRIVER DRIVER(                                    \
    RESET(regs_reset)                                          \
    SST("REGS", regs))

#endif /* _CORE_RIA_REGS_H_ */
