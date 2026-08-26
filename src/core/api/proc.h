/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_PROC_H_
#define _CORE_API_PROC_H_

/* The process manager handles argv and launching other ROMs.
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

/* Platforms that stage their own next program: true when an exec is
 * waiting and its image has been loaded, so the caller starts the
 * machine again. Consumed by the call. */
bool proc_exec_take(void);

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
 * run -- and for the machines whose EXIT handler reads it themselves. */
void proc_set_exit_code(int16_t code);

/* ---- what a machine answers about starting the next program ---- */

/* Op 0x09 asked for a new program: commit to it, stopping whatever this
 * machine has to stop. The argv buffer already holds what it will be given. */
void proc_exec_start(const char *path);

/* The launcher is being re-run from inside a stop that is already underway,
 * so this one only commits the load. */
void proc_exec_relaunch(const char *path);

/* True while a load this machine has already committed to is on its way, so
 * the chain must not schedule another over it. */
bool proc_exec_inflight(void);

// Load a ROM via NFC
void proc_nfc(const uint8_t *tag_data, size_t len);

#endif /* _CORE_API_PROC_H_ */
