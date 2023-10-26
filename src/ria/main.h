/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MAIN_H_
#define _MAIN_H_

#include <stdint.h>
#include <stdbool.h>

/* This is the main kernel event loop.
 */

// Request to "start the 6502".
// It will safely do nothing if the 6502 is already running.
void main_run(void);

// Request to "stop the 6502".
// It will safely do nothing if the 6502 is already stopped.
void main_stop(void);

// Request to "break the system".
// A break is triggered by CTRL-ALT-DEL and UART breaks.
// If the 6502 is running, stop events will be called first.
// Kernel modules should reset to a state similar to after
// init() was first run.
void main_break(void);

// This is true when the 6502 is running or there's a pending
// request to start it.
bool main_active(void);

/* Special events dispatched in main.c
 */

void main_task(void);
void main_reclock(uint32_t sys_clk_khz, uint16_t clkdiv_int, uint8_t clkdiv_frac);
bool main_pix(uint8_t ch, uint8_t addr, uint16_t word);
bool main_api(uint8_t operation);

/* All pin assignments
 */

#define AUD_L_PIN 28
#define AUD_R_PIN 27

#define CPU_RESB_PIN 26
#define CPU_IRQB_PIN 22
#define CPU_PHI2_PIN 21

#define PIX_PIN_BASE 0 /* PIX0-PIX3 */

#define RIA_PIN_BASE 6
#define RIA_CS_PIN (RIA_PIN_BASE + 0)
#define RIA_RWB_PIN (RIA_PIN_BASE + 1)
#define RIA_DATA_PIN_BASE (RIA_PIN_BASE + 2)  /* D0-D7 */
#define RIA_ADDR_PIN_BASE (RIA_PIN_BASE + 10) /* A0-A4 */

/* All resource assignments
 */

#define AUD_PWM_IRQ_PIN 14 /* No IO */

#define COM_UART uart1
#define COM_UART_BAUD_RATE 115200
#define COM_UART_TX_PIN 4
#define COM_UART_RX_PIN 5

#define PIX_PIO pio1
#define PIX_SM 1

#define RIA_WRITE_PIO pio0
#define RIA_WRITE_SM 0
#define RIA_READ_PIO pio0
#define RIA_READ_SM 1
#define RIA_ACT_PIO pio1
#define RIA_ACT_SM 0

#define VGA_BACKCHANNEL_PIN COM_UART_TX_PIN
#define VGA_BACKCHANNEL_BAUDRATE 115200
#define VGA_BACKCHANNEL_PIO pio0
#define VGA_BACKCHANNEL_SM 2

#endif /* _MAIN_H_ */
