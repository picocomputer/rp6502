/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_PROC_H_
#define _HOST_POCKET_SW_PROC_H_

#include "core/api/api.h"
#include "core/api/proc.h"

#include <stdint.h>

/* proc_restage blocks on Get File and must be called with the machine
 * stopped. */
void proc_restage(void);

bool proc_exec_take(void);

/* Drops an exec on its way and the launcher, so nothing starts by itself. */
void proc_exec_drop(void);

/* The path a start by name staged, the exec on its way, then core/api/proc.h's
 * chain. */
#define PROC_SST_SIZE (API_PATH_MAX + 1 + 1 + PROC_CHAIN_SST_SIZE)
void proc_sst_save(sst_cursor_t *c, unsigned flags);
bool proc_sst_load(sst_cursor_t *c, unsigned flags);

#define PROC_DRIVER DRIVER(                                    \
    RUN(proc_run)                                              \
    STOP(proc_stop)                                            \
    SST(.id = "PROC",                                          \
        .version = 1,                                          \
        .size = PROC_SST_SIZE,                                 \
        .save = proc_sst_save,                                 \
        .load = proc_sst_load))

#endif /* _HOST_POCKET_SW_PROC_H_ */
