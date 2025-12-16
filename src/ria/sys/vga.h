/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_VGA_H_
#define _RIA_SYS_VGA_H_

/* Communications with RP6502-VGA.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define VGA_BACKCHANNEL_PIN COM_UART_TX_PIN
#define VGA_BACKCHANNEL_BAUDRATE 115200
#define VGA_BACKCHANNEL_PIO pio1
#define VGA_BACKCHANNEL_SM 2

/* Main events
 */

void vga_init(void);
void vga_task(void);
void vga_run(void);
void vga_stop(void);
void vga_break(void);
void vga_post_reclock(uint32_t sys_clk_khz);

// Fully connected with backchannel.
bool vga_connected(void);

// For monitor status command.
void vga_print_status(void);

// Configuration setting VGA
void vga_load_display_type(const char *str, size_t len);
bool vga_set_display_type(uint8_t display_type);
uint8_t vga_get_display_type(void);

#endif /* _RIA_SYS_VGA_H_ */
