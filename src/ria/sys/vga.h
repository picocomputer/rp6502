/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_VGA_H_
#define _RIA_SYS_VGA_H_

/* Connunications with RP6502-VGA.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define VGA_BACKCHANNEL_PIN 4 // COM_UART_TX_PIN
#define VGA_BACKCHANNEL_BAUDRATE 115200
#define VGA_BACKCHANNEL_PIO pio1
#define VGA_BACKCHANNEL_SM 2

/* Kernel events
 */

void vga_init(void);
void vga_task(void);
void vga_run(void);
void vga_stop(void);
void vga_reset(void);
void vga_post_reclock(uint32_t sys_clk_khz);

// Active at startup and when reconnecting.
bool vga_active(void);

// Backchannel messages are being received.
bool vga_backchannel(void);

// For monitor status command.
void vga_print_status(void);

// Config handler.
bool vga_set_vga(uint32_t display_type);

#endif /* _RIA_SYS_VGA_H_ */
