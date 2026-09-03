/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_VGA_H_
#define _VGA_SYS_VGA_H_

#include "core/sys/driver.h"

/* Video Graphics Array
 */

#include "core/vga/vga.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void vga_init(void);
void vga_task(void);

/* Start core 1 rendering. main.c calls this after the driver walk, because
 * core 1 draws the terminal and the terminal has to exist first. */
void vga_start_render_core(void);

// Display type. Choose SD for 4:3 displays,
// HD for 16:9 displays, and SXGA for 5:4 displays.
typedef enum
{
    vga_sd,   // 640x480 (480p) default
    vga_hd,   // 640x480 and 1280x720 (720p)
    vga_sxga, // 1280x1024 (5:4)
} vga_display_t;

void vga_set_display(vga_display_t display);
void vga_xreg_canvas(uint16_t *xregs);
/* This driver's row in a machine's driver list; see core/sys/driver.h. Programs the console canvas, which asks the terminal its height, so it
 * follows TERM and FONT. */
#define VGA_DRIVER DRIVER(vga_init, vga_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _VGA_SYS_VGA_H_ */
