/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_API_PROC_EXEC_H_
#define _CORE_API_PROC_EXEC_H_

#include "core/api/proc.h"
#include <stdbool.h>
#include <stdint.h>

/* Seed the initial program's argv: its own path + args. False on overflow. */
bool proc_set_argv(const char *rom, int argc, char *const *args);
void proc_init(void); /* clear any pending exec (cold boot) */

/* Request an exec: load rom_path (a host/drive path or overlay ROM name) as the
 * new program at the next frame boundary. Stops the current program; the frame
 * proc_exec_task() commits it. */
void proc_exec(const char *rom_path);
bool proc_exec_pending(void);     /* an exec is queued but not yet committed */

/* Perform a queued exec: stop, load, run -- all through the lifecycle. */
void proc_exec_task(void);

/* Launcher chain (firmware proc.h), reached by the vendored atr.c through the
 * LAUNCHER/EXIT_CODE attributes. A launcher re-runs after each child exits;
 * proc_exit schedules that re-exec. */
bool proc_exit(int16_t exit_code);     /* true if a launcher re-exec was scheduled */

/* This driver's machine-lifecycle row; see core/lifecycle.h. The pending-exec queue
 * is its own driver, separate from core/api/proc.c's row -- they share a
 * prefix and nothing else, which is why this is named for the file. */
#define PROC_EXEC_MACH_LIFECYCLE LIFECYCLE(proc_init, nul_task, proc_exec_task, nul_run, nul_stop, nul_break)

#endif /* _CORE_API_PROC_EXEC_H_ */
