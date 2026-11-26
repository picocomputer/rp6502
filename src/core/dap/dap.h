/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * C entry points for the cppdap server in dap.cpp, which speaks the Debug
 * Adapter Protocol over stdin and stdout. Built only under EMU_WITH_DEBUGGER.
 */

#ifndef _CORE_DAP_DAP_H_
#define _CORE_DAP_DAP_H_

#include <stdarg.h>

/* The host's lock on the machine, and a wake for the thread that runs it. A
 * handler on cppdap's thread takes the lock to read or write the machine;
 * work queued for dap_pump, a pause and the end of configuration wake it.
 * Call before dap_start. Without them the handlers take no lock. */
void dap_set_host(void (*lock)(void), void (*unlock)(void), void (*wake)(void));

/* Call once, after sys_init. cppdap reads messages on a thread of its own, so a
 * handler either queues its work for dap_pump or takes the machine lock. */
void dap_start(void);

bool dap_is_active(void);

/* The ROM arguments a launch request falls back on. Call before dap_start. */
void dap_set_default_args(int argc, char **argv);

/* Applies the queued DAP requests and emits the launch and termination events.
 * Call once per step, on the thread that runs the machine, under its lock. */
void dap_pump(void);

/* Sends what the machine's side has for the client: events, its output and
 * the answers to queued requests, in the order they were made. Call without
 * the machine lock, since a client slow to read holds up the caller. */
void dap_send(void);

/* True once the client has disconnected, which asks the window loop to quit. */
bool dap_quit_requested(void);

void dap_stop(void);

/* A machine log line, sent to the client as an OutputEvent in the "stderr"
 * category beside the program's own output. The append holds the output
 * buffer's mutex, so any thread may call this. */
void dap_log(int level, const char *category, const char *fmt, va_list ap);

#endif /* _CORE_DAP_DAP_H_ */
