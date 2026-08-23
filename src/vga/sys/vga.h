/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_VGA_H_
#define _VGA_SYS_VGA_H_

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
#endif /* _VGA_SYS_VGA_H_ */
