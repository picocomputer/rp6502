/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_H_
#define _VGA_H_

#include <stdint.h>
#include <stdbool.h>

/* Kernel events
 */

void vga_init(void);
void vga_task(void);
void vga_run(void);
void vga_stop(void);
void vga_reset(void);
void vga_reclock(uint32_t sys_clk_khz);

// Active at startup and when reconnecting.
bool vga_active(void);

bool vga_backchannel(void);
void vga_print_status(void);

// Config handler.
bool vga_set_vga(uint32_t display_type);

#endif /* _VGA_H_ */
