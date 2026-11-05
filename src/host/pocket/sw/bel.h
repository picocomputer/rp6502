/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_BEL_H_
#define _HOST_POCKET_SW_BEL_H_

#include <stdint.h>

void bel_init(void);
void bel_task(void);
/* Moves the bell's deadlines by a wake's length in microseconds. */
void bel_wake(uint64_t us);

#define BEL_DRIVER DRIVER(nul_init, bel_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _HOST_POCKET_SW_BEL_H_ */
