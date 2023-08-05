/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_H_
#define _VGA_H_

#include <stdbool.h>

void vga_init(void);
void vga_task(void);
bool vga_active(void);
bool vga_backchannel(void);

#endif /* _VGA_H_ */
