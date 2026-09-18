/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_PROC_H_
#define _HOST_POCKET_SW_PROC_H_

#include "core/api/proc.h"

#include <stdint.h>

/* proc_restage blocks on Get File and must be called with the machine
 * stopped. */
void proc_restage(void);

const char *proc_staged_path(void);

bool proc_exec_take(void);

#define PROC_DRIVER DRIVER(nul_init, nul_task, nul_task, proc_run, proc_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _HOST_POCKET_SW_PROC_H_ */
