/*
 * Copyright (c) 2026 Rumbledethumps
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

// Fully connected with backchannel.
bool vga_connected(void);

// Responders for status.
int vga_boot_response(char *buf, size_t buf_size, int state, unsigned width);
int vga_status_response(char *buf, size_t buf_size, int state, unsigned width);

// Configuration setting VGA
void vga_load_display_type(const char *str);
bool vga_set_display_type(uint8_t display_type);
uint8_t vga_get_display_type(void);
const char *vga_get_display_type_verbose(void);

// Selected VGA canvas. Mirrors vga_canvas_t in src/vga/sys/vga.h. The RIA side
// shadows the VGA side's selection so callers (e.g. rln) can pick text layout
// without reaching across the PIX bus.
typedef enum
{
    vga_canvas_console = 0,
    vga_canvas_320_240,
    vga_canvas_320_180,
    vga_canvas_640_480,
    vga_canvas_640_360,
} vga_canvas_t;

vga_canvas_t vga_get_canvas(void);
void vga_set_canvas(uint16_t canvas_word);

// VGA-bound protocol state pushed from other subsystems
void vga_set_tel_console_active(bool active);
void vga_set_code_page(uint16_t cp);

#endif /* _RIA_SYS_VGA_H_ */
