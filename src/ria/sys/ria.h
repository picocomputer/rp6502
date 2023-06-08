/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_H_
#define _RIA_H_

/*
 * RP6502 Interface Adapter for WDC W65C02S.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Content of these 15 pins is bound to the PIO program structure.
#define RIA_PIN_BASE 6
#define RIA_CS_PIN (RIA_PIN_BASE + 0)
#define RIA_RWB_PIN (RIA_PIN_BASE + 1)
#define RIA_DATA_PIN_BASE (RIA_PIN_BASE + 2)
#define RIA_ADDR_PIN_BASE (RIA_PIN_BASE + 10)

// Kernel events
void ria_init();
void ria_task();
void ria_run();
void ria_stop();
void ria_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac);

// Move data from the 6502 to ria_buf.
void ria_read_buf(uint16_t addr);

// Move data from ria_buf to the 6502.
void ria_write_buf(uint16_t addr);

// Verify the ria_buf matches 6502 memory.
void ria_verify_buf(uint16_t addr);

// The RIA is active when it's performing an ria_buf action.
bool ria_active();

// Prints a "?" error and returns true if last ria_buf action failed.
bool ria_print_error_message();

// Registers are located at the bottom of cpu1 stack.
// cpu1 runs the action loop and uses very little stack.
extern uint8_t ria_regs[0x20];
#define REGS(addr) ria_regs[(addr)&0x1F]
#define REGSW(addr) ((uint16_t *)&REGS(addr))[0]
asm(".equ ria_regs, 0x20040000");

// Misc memory buffer for moving things around.
// 6502 <-> RAM, USB <-> RAM, UART <-> RAM, etc.
#define MBUF_SIZE 1024
extern uint8_t ria_buf[MBUF_SIZE];
extern size_t ria_buf_len;

// Compute CRC32 of ria_buf to match zlib.
uint32_t ria_buf_crc32();

#endif /* _RIA_H_ */
