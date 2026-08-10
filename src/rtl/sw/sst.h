/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_SST_H_
#define _FPGA_SW_SST_H_

#include <stdbool.h>

/* True while the host has written a blob into the window but the
 * restore has not landed yet. Asked once at boot, where it means: do
 * not start the ROM, one is coming. */
bool sst_pending(void);

/* Polled from the loop. Does nothing at all until a restore has
 * happened, and then does it once. */
void sst_task(void);

#endif /* _FPGA_SW_SST_H_ */
