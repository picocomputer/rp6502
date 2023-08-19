/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_H_
#define _VGA_H_

#include <stdint.h>
#include <stdbool.h>

void vga_init(void);
void vga_task(void);
void vga_run(void);
void vga_reclock(uint32_t sys_clk_khz);
bool vga_active(void);
bool vga_backchannel(void);
void vga_print_status(void);

#endif /* _VGA_H_ */
