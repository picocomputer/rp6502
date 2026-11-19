/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_PROC_H_
#define _HOST_POCKET_SW_PROC_H_

#include "core/api/api.h"
#include "core/api/proc.h"
#include "core/sys/sst.h"

#include <stdint.h>

/* proc_restage blocks on Get File and must be called with the machine
 * stopped. */
void proc_restage(void);

bool proc_exec_take(void);

/* Drops an exec on its way and the launcher, so nothing starts by itself. */
void proc_exec_drop(void);

extern const sst_table_t proc_pocket_sst;

/* After a failed savestate load: no exec on its way and proc_chain_reset. */
void proc_reset(void);

#define PROC_DRIVER DRIVER(                                    \
    RUN(proc_run)                                              \
    STOP(proc_stop)                                            \
    RESET(proc_reset)                                          \
    SST("PROC", proc_pocket))

#endif /* _HOST_POCKET_SW_PROC_H_ */
