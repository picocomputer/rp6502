/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_VGA_H_
#define _RIA_SYS_VGA_H_

/* Communications with RP6502-VGA.
 */

#include "core/vga/vga.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define VGA_BACKCHANNEL_PIN COM_UART_TX_PIN
#define VGA_BACKCHANNEL_BAUDRATE 115200
#define VGA_BACKCHANNEL_PIO pio1
#define VGA_BACKCHANNEL_SM 2

// Start bit trap for VSYNC. pio0 for its guaranteed GPIO base of 0; pio1 has
// no instruction memory left and pio2 belongs to the cyw43 claim. Inputs need
// no funcsel, so this watches the same pin as the receiver above.
#define VGA_VSYNC_TRAP_PIO pio0
#define VGA_VSYNC_TRAP_SM 3
#define VGA_VSYNC_TRAP_IRQ PIO0_IRQ_0

// Trap armed for this much either side of the expected VSYNC. Must stay
// inside the VGA's RIA_VSYNC_LOCKOUT_US by more than a byte time plus skew.
#define VGA_VSYNC_PERIOD_US 16667
#define VGA_VSYNC_WINDOW_US 1500

// Start bit to the receiver's autopush: 1 PIO cycle for the VGA to leave idle
// plus 68 cycles of ours. Back-dates the byte to when the VGA sent it.
#define VGA_VSYNC_TRANSIT_US 75

/* Main events
 */

void vga_init(void);
void vga_task(void);
void vga_run(void);
void vga_stop(void);
void vga_break(void);

// Fully connected with backchannel.
bool vga_connected(void);

// Responders for status.
int vga_boot_response(char *buf, size_t buf_size, int state, unsigned width);
int vga_status_response(char *buf, size_t buf_size, int state, unsigned width);

// Configuration setting VGA
bool vga_check_display_type(uint8_t *v);
void vga_apply_display_type(uint8_t display_type, bool changed);
int vga_display_type_response(char *buf, size_t buf_size, int state, unsigned width);
bool vga_set_display_type(uint8_t display_type);
const char *vga_get_display_type_verbose(void);

void vga_set_canvas(uint16_t canvas_word);

// VGA-bound protocol state pushed from other subsystems
void vga_set_tel_console_active(bool active);

/* This driver's row in a machine's driver list; see core/sys/driver.h. After PIX in the driver list:
 * vga_init's first act is to disable the backchannel, which is a PIX message,
 * and its connect blocks on the bus RIA brought up. */
#define VGA_CONFIG_DISPLAY_TYPE CONFIG_INT(D, vga, display_type, uint8_t, 0, \
    vga_check_display_type, vga_apply_display_type, STR_VGA, \
    vga_display_type_response, STR_HELP_SET_VGA, NULL)
#define VGA_DRIVER DRIVER(vga_init, vga_task, nul_task, vga_run, vga_stop, vga_break, \
    VGA_CONFIG_DISPLAY_TYPE, nul_config)

#endif /* _RIA_SYS_VGA_H_ */
