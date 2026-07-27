/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's windows on the machine, as mapped in rp6502.sv and
 * rv_soc.sv. Byte windows by design; the register cells are true words.
 */

#ifndef _FPGA_SW_MMIO_H_
#define _FPGA_SW_MMIO_H_

#include <stdint.h>

#define MMIO_CONSOLE (*(volatile uint32_t *)0xF0000000u)
#define MMIO_KBD (*(volatile uint32_t *)0xF0000008u)

#define SRAM ((volatile uint8_t *)0x10000000u)
#define REGS_WIN ((volatile uint8_t *)0x20000000u)
#define UART_POP (*(volatile uint32_t *)0x20000040u)
#define RX_OFFER (*(volatile uint32_t *)0x20000048u)
#define CPU_RUN (*(volatile uint8_t *)0x40000000u)
#define API_PENDING (*(volatile uint8_t *)0x40000004u)

#endif /* _FPGA_SW_MMIO_H_ */
