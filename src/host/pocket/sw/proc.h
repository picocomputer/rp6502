/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_PROC_H_
#define _HOST_POCKET_SW_PROC_H_

#include "core/api/proc.h"

#include <stdint.h>

/* Ask what the staged image is called and make that argv, dropping any
 * exec still waiting: a program the user picked supersedes the one the
 * outgoing program asked for. Blocking, machine stopped. */
void proc_restage(void);

/* The image the machine is actually running, as the host spelled it.
 * After a restore this is the blob's answer, which is the program the
 * restored session belongs to -- not whatever the host has slot 0 bound
 * to now. Empty before anything has been staged. */
const char *proc_staged_path(void);

/* An exec is waiting and its image is staged, so the caller starts the
 * machine again. Consumed by the call. This machine alone stages its own
 * next program, so this is its own. */
bool proc_exec_take(void);

/* This machine's proc row; see core/sys/driver.h. The chain's columns over
 * core/api/proc.c; the load is main.c's, after the stop. */
#define PROC_DRIVER DRIVER(nul_init, nul_task, nul_task, proc_run, proc_stop, nul_break, nul_config, nul_config)

#endif /* _HOST_POCKET_SW_PROC_H_ */
