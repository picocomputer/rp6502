/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_SYS_PRO_H_
#define _CORE_SYS_PRO_H_

#include "core/api/pro.h"
#include <stdbool.h>
#include <stdint.h>

/* Seed the initial program's argv: its own path + args. False on overflow. */
bool pro_set_argv(const char *rom, int argc, char *const *args);
void pro_init(void); /* clear any pending exec (cold boot) */

/* Request an exec: load rom_path (a host/drive path or overlay ROM name) as the
 * new program at the next frame boundary. Stops the current program; the frame
 * loop commits it via pro_take_exec(). */
void pro_exec(const char *rom_path);
const char *pro_take_exec(void); /* the pending exec path, cleared, else NULL */
bool pro_exec_pending(void);     /* an exec is queued but not yet committed */

/* Launcher chain (firmware pro.h), reached by the vendored atr.c through the
 * LAUNCHER/EXIT_CODE attributes. A launcher re-runs after each child exits;
 * pro_exit schedules that re-exec. */
bool pro_exit(int16_t exit_code);     /* true if a launcher re-exec was scheduled */

#endif /* _CORE_SYS_PRO_H_ */
