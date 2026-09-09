/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The console this process was launched from, which is the host's console and
 * not the machine's. The file is console.h and not con.h because CON is a
 * reserved device name on Windows.
 */

#ifndef _OSAL_CONSOLE_H_
#define _OSAL_CONSOLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void os_console_attach(void);
bool os_console_is_terminal(void);
bool os_console_stdin_is_terminal(void);
bool os_console_stderr_is_terminal(void);

/* Raw mode hands the keystrokes over as they are struck, Ctrl-C included. One
 * key is left able to interrupt even in raw mode -- Ctrl-\ on a POSIX terminal
 * and Ctrl-Break at a Windows console -- because that is the only way to stop
 * a machine that has stopped reading. Turning raw mode off gives the terminal
 * back as it was found. */
void os_console_raw(bool on);

/* Never waits, and returns 0 when nothing is ready. End of file becomes
 * visible through os_console_ended only after a read has taken the last bytes,
 * so a reader cannot see the end before it has consumed everything. */
size_t os_console_read(char *buf, size_t count);
bool os_console_ended(void);

/* Waits until input is ready or the deadline passes. False when the whole wait
 * passed and nothing became ready. */
bool os_console_wait(uint64_t ns);

/* A break is the host asking the machine to stop from outside the program it
 * is running: Ctrl-Break at a Windows console, Ctrl-\ at a POSIX one, and a
 * hung-up stdout. This runs in a signal handler or on another thread, so it
 * only sets a flag and the thread that owns the machine stops it. A second
 * interrupt is the hard way out, for a machine too wedged to reach its own
 * teardown. */
bool os_console_break_asked(void);
void os_console_break_ask(void);

/* Ends the process the way the interrupt would have, once the machine is down,
 * so a make or a shell loop sees a run that was interrupted rather than one
 * that merely failed. Does not return. */
void os_console_break_exit(void);

#endif /* _OSAL_CONSOLE_H_ */
