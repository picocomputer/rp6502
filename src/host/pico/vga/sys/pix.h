/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_PIX_H_
#define _VGA_SYS_PIX_H_

#include "core/sys/driver.h"

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

/* This driver's row in a machine's driver list; see core/sys/driver.h. Last: an xreg can arrive the instant its DMA is live and dispatches
 * into every driver above, and scanvideo must claim DMA 0-2 first. */
#define PIX_DRIVER DRIVER(pix_init, pix_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _VGA_SYS_PIX_H_ */
