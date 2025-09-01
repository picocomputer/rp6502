/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_PIX_H_
#define _VGA_SYS_PIX_H_

/* Listens on the PIX bus
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define PIX_PIO pio1
#define PIX_REGS_SM 1
#define PIX_XRAM_SM 2
#define PIX_PHI2_PIN 11

/* Main events
 */

void pix_init(void);
void pix_task(void);

#endif /* _VGA_SYS_PIX_H_ */
