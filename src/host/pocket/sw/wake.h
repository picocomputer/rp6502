/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_WAKE_H_
#define _HOST_POCKET_SW_WAKE_H_

#include <stdbool.h>

bool wake_pending(void);

void wake_task(void);

#define WAKE_DRIVER DRIVER(nul_init, nul_task, wake_task, nul_run, nul_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _HOST_POCKET_SW_WAKE_H_ */
