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

/* A program stopped. Unless an exec is on its way, the launcher runs again,
 * or with none, the chain ends. */
void proc_stop(void);

/* The program is done: the machine returns to its monitor, or with none,
 * idles. The launcher goes, the code page goes back, the type-ahead is
 * dropped and the console gets its soft reset on a clean line. */
void proc_chain_end(void);

/* An exec or a relaunch did not load. The chain ends with exit code 1. */
void proc_exec_failed(void);

/* argv[0] of the program running now, an empty string between programs. */
const char *proc_running(void);

bool proc_api_argv(void);
bool proc_api_exec(void);

/* A program that sets the launcher flag registers itself as the one to come
 * back to, so proc_stop re-execs it when a later program stops. The chain ends
 * when the launcher itself stops, when the flag is cleared, or at a break.
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

/* A break: the program and its launcher chain end. */
void proc_break(void);

/* Ctrl-Alt-Delete. False on a machine with no monitor, where the key types. */
bool proc_break_to_monitor(void);

/* Alt-F4: the program exits as exit(-1) does. False when nothing runs, from the
 * launcher itself, and with no launcher on a machine with no monitor, where the
 * key types. */
bool proc_quit(void);

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
