/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_SST_H_
#define _HOST_POCKET_SW_SST_H_

#include <stdbool.h>

/* True while the host has written a blob into the window but the
 * restore has not landed yet. Asked once at boot, where it means: do
 * not start the ROM, one is coming. */
bool sst_pending(void);

/* Polled from the loop. Does nothing at all until a restore has
 * happened, and then does it once. */
void sst_task(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. The savestate engine: it reads and writes the slot, so it is not safe
 * during file IO and runs after api in the io column. */
#define SST_DRIVER DRIVER(nul_init, nul_task, sst_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _HOST_POCKET_SW_SST_H_ */
