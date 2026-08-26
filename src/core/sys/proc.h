/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_SYS_PROC_H_
#define _CORE_SYS_PROC_H_

#include "core/api/proc.h"
#include <stdbool.h>
#include <stdint.h>

/* Seed the initial program's argv: its own path + args. False on overflow. */
bool proc_set_argv(const char *rom, int argc, char *const *args);
void proc_init(void); /* clear any pending exec (cold boot) */

/* Request an exec: load rom_path (a host/drive path or overlay ROM name) as the
 * new program at the next frame boundary. Stops the current program; the frame
 * loop commits it via proc_take_exec(). */
void proc_exec(const char *rom_path);
const char *proc_take_exec(void); /* the pending exec path, cleared, else NULL */
bool proc_exec_pending(void);     /* an exec is queued but not yet committed */

/* Launcher chain (firmware proc.h), reached by the vendored atr.c through the
 * LAUNCHER/EXIT_CODE attributes. A launcher re-runs after each child exits;
 * proc_exit schedules that re-exec. */
bool proc_exit(int16_t exit_code);     /* true if a launcher re-exec was scheduled */

#endif /* _CORE_SYS_PROC_H_ */
