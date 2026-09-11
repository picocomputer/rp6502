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

/* Call once, after sys_init. cppdap reads messages on a thread of its own, so a
 * handler either queues its work for dap_pump or reads machine state that is
 * still because the CPU is stopped. */
void dap_start(void);

bool dap_is_active(void);

/* The ROM arguments a launch request falls back on. Call before dap_start. */
void dap_set_default_args(int argc, char **argv);

/* Applies the queued DAP requests and emits the launch and termination events.
 * Call once per frame, on the thread that runs the machine. */
void dap_pump(void);

/* True once the client has disconnected, which asks the window loop to quit. */
bool dap_quit_requested(void);

void dap_stop(void);

/* A machine log line, sent to the client as an OutputEvent in the "stderr"
 * category beside the program's own output. The append holds the output
 * buffer's mutex, so any thread may call this. */
void dap_log(int level, const char *category, const char *fmt, va_list ap);

#endif /* _CORE_DAP_DAP_H_ */
