/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_PROC_H_
#define _CORE_API_PROC_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void proc_run(void);

/* A program stopped. True when the machine keeps running -- an exec already in
 * flight, or a launcher relaunch -- false when the chain has ended. */
bool proc_stop(void);

/* argv[0] of the program running now, an empty string between programs. */
const char *proc_running(void);

bool proc_api_argv(void);
bool proc_api_exec(void);

/* A program that sets the launcher flag registers itself as the one to come
 * back to, so proc_stop re-execs it when a later program stops. The chain ends
 * when the launcher itself stops or when the flag is cleared.
 */
void proc_cancel_launcher(void);
bool proc_has_launcher(void);
void proc_set_launcher(bool is_launcher);
bool proc_is_launcher(void);
int16_t proc_get_exit_code(void);
void proc_set_exit_code(int16_t code);

/* The two paths a savestate carries. A load puts them back here rather than
 * through proc_run, because proc_run takes argv[0] from the argument buffer
 * and the load has not restored that buffer yet. */
const char *proc_launcher(void);
void proc_restore_paths(const char *running, const char *launcher);

/* Program EXIT, op 0xFF: record the code and stop. */
void proc_exit(int16_t exit_code);

/* Each machine answers these three for itself.
 */

/* Op 0x09 asked for a new program: commit to it, stopping whatever this
 * machine has to stop. argv[0] already names it. */
void proc_exec_start(void);

/* The launcher is being re-run from inside a stop that is already underway,
 * so this one only commits the load. argv[0] is the launcher's path. */
void proc_exec_relaunch(void);

/* True while a load this machine has already committed to is on its way, so
 * that nothing schedules another over it. */
bool proc_exec_inflight(void);

#endif /* _CORE_API_PROC_H_ */
