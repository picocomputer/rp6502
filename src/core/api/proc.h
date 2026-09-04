/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_PROC_H_
#define _CORE_API_PROC_H_

/* The launcher chain: which program is running, which one to return to when
 * it exits, and what it exited with. Every machine runs the same rules, so
 * they are here once; how a machine actually starts the next program is the
 * seam at the bottom, which each answers its own way. The driver row is the
 * machine's too: its own proc header lists the columns its proc.c fills over
 * these.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void proc_run(void);

/* A program stopped. True when the launcher was asked for and the machine
 * keeps running; false when the chain has ended. */
bool proc_stop(void);

/* argv[0] of what is running now, empty between programs. */
const char *proc_running(void);

/* The API implementation
 */

bool proc_api_argv(void);
bool proc_api_exec(void);

/* Launcher: when set, proc_stop() will re-exec the launcher ROM.
 * The chain breaks when the launcher itself stops or on proc_cancel_launcher().
 */
void proc_cancel_launcher(void);
bool proc_has_launcher(void);
void proc_set_launcher(bool is_launcher);
bool proc_is_launcher(void);
int16_t proc_get_exit_code(void);

/* The code a program returned, for a stop that did not come through the
 * EXIT syscall -- a failed exec, where the machine halts with nothing to
 * run. */
void proc_set_exit_code(int16_t code);

/* Program EXIT (op 0xFF): record the code and stop. What happens next is the
 * chain's, decided by the stop walk -- a launcher to go back to, or nothing
 * left to run. */
void proc_exit(int16_t exit_code);

/* ---- what a machine answers about starting the next program ---- */

/* Op 0x09 asked for a new program: commit to it, stopping whatever this
 * machine has to stop. argv[0] already names it. */
void proc_exec_start(void);

/* The launcher is being re-run from inside a stop that is already underway,
 * so this one only commits the load; argv[0] is the launcher's path. */
void proc_exec_relaunch(void);

/* True while a load this machine has already committed to is on its way, so
 * the chain must not schedule another over it. */
bool proc_exec_inflight(void);

#endif /* _CORE_API_PROC_H_ */
