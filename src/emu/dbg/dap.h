/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * C-callable entry points for the cppdap-based DAP server (dap.cpp). The server
 * speaks the Debug Adapter Protocol over stdin/stdout (the channel VS Code's
 * lldb-dap host uses to drive us). Only built/called under EMU_WITH_DEBUGGER in
 * --dap mode.
 */

#ifndef _EMU_DAP_H_
#define _EMU_DAP_H_

#ifdef __cplusplus
extern "C"
{
#endif

/* Create the DAP session, register handlers, and bind it to stdin/stdout.
 * cppdap runs the message reader on its own thread; handlers either marshal work
 * to the main loop (via dap_pump) or read machine state while the CPU is
 * stopped. Call once, after emu_init(). */
void dap_start(void);

/* Main-thread service: apply queued DAP requests against the dbg engine and the
 * machine, and emit the launch-sequence / termination events. Call once per
 * frame from the window loop. */
void dap_pump(void);

/* True once the client has sent a Disconnect request — the window loop should
 * then close the app. */
bool dap_quit_requested(void);

/* Close the session (window teardown). */
void dap_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_DAP_H_ */
