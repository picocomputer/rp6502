/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_VGA_H_
#define _FPGA_SW_VGA_H_

#include <stdint.h>
#include <stdbool.h>

/* Both sides of the VGA contract on one machine, the emu/sys/vga.h shape:
 * the RIA-side status the shared readline consults, and the VGA-side prog
 * layer the mode programs call. Self-contained like the emulator's header
 * because ria/sys/vga.h and vga/sys/vga.h each define their own canvas
 * enum; definitions here stay ABI-compatible with both.
 */

typedef enum
{
    vga_canvas_console = 0,
    vga_canvas_320_240,
    vga_canvas_320_180,
    vga_canvas_640_480,
    vga_canvas_640_360,
} vga_canvas_t;

bool vga_connected(void);
vga_canvas_t vga_get_canvas(void);
uint8_t vga_get_display_type(void);
int16_t vga_canvas_height(void);
int16_t vga_vsync_scanline(void);

/* The mode dispatch announces what the next vga_prog_fill publishes —
 * a fill-function pointer means nothing to hardware. */
void vga_prog_mode(uint8_t mode, uint16_t attr);
bool vga_set_canvas(uint16_t canvas);

#endif /* _FPGA_SW_VGA_H_ */
