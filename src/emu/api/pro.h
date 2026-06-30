/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Program / argv (pro.c): argv passing + exec, and the launcher chain the
 * vendored atr.c reaches through the LAUNCHER/EXIT_CODE attributes. Mirrors the
 * firmware api/pro.c.
 */

#ifndef _EMU_PRO_H_
#define _EMU_PRO_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

bool pro_api_argv(void);              /* op 0x08: read argv onto the xstack */
bool pro_api_exec(void);              /* op 0x09: replace the program */
void pro_set_argv0(const char *path); /* seed argv[0] for the initial program */
void pro_run(void);                   /* snapshot argv[0] of the starting program */

/* Launcher chain (firmware pro.h), reached by the vendored atr.c through the
 * LAUNCHER/EXIT_CODE attributes. A launcher re-runs after each child exits;
 * pro_exit schedules that re-exec. */
bool pro_has_launcher(void);
void pro_set_launcher(bool is_launcher);
bool pro_is_launcher(void);
int16_t pro_get_exit_code(void);
bool pro_exit(int16_t exit_code); /* true if a launcher re-exec was scheduled */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_PRO_H_ */
