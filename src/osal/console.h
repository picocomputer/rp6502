/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The console this process was launched from -- the host's, not the machine's.
 * What is here is only what the operating system can answer: is there a
 * terminal, is it the same one both ways, hand the keystrokes over as they
 * are struck, what has arrived, has it ended, and did the user ask to stop.
 * Whether that terminal *is* the machine's console is decided above this,
 * from these answers.
 *
 * Its own file, and its own per-OS files, because this is the only part of
 * the OS layer that takes something from the process: signal dispositions, an
 * atexit, and a terminal it must give back. A machine that is a guest in
 * someone else's process -- a libretro core, an APK, a browser tab -- has no
 * console, may install none of that, and links none of this.
 *
 * Not con.h: CON is a reserved device name on Windows, extension and all.
 */

#ifndef _OSAL_CONSOLE_H_
#define _OSAL_CONSOLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Stand this process's stdio up, before anything can use it. Reattach to the
 * parent console where the OS needs it, so a windowed build launched from a
 * terminal can still answer --help; repair any of the three descriptors the
 * launcher closed, or the first file the machine opens becomes fd 0 and the
 * console reads it; and arm the ask below, which every run needs and not only
 * one that takes a terminal raw. */
void os_console_attach(void);

/* One terminal, both ways: a terminal on stdin with a file on stdout is a
 * pipeline, and the machine's screen does not belong in the file. This
 * decides who owns stdout. */
bool os_console_is_terminal(void);

/* Whether stdin alone is a terminal, which decides what stdin is: a console
 * someone types at, taken raw, or a stream to be read as it is consumed. */
bool os_console_stdin_is_terminal(void);

/* Whether stderr is that same terminal, which is what decides whether a
 * second copy of the program's stderr would print everything twice. */
bool os_console_stderr_is_terminal(void);

/* Hand the keystrokes over as they are struck and give the machine every byte
 * it can, Ctrl-C included. What raw mode must not take is the one way out
 * when the machine has stopped listening, so Ctrl-\ keeps its signal.
 * Turning it off gives the terminal back as it was found. */
void os_console_raw(bool on);

/* Never waits: what is ready now, and 0 when nothing is, as UTF-8 whatever
 * the host stores natively. End of file is latched here rather than reported
 * separately, so a reader cannot ask before the last bytes are taken. */
size_t os_console_read(char *buf, size_t count);
bool os_console_ended(void);

/* Wait until something is ready or the deadline passes, for a host with
 * nothing else to do meanwhile. False if it waited the whole time for
 * nothing. */
bool os_console_wait(uint64_t ns);

/* The host asking the machine to stop from outside the program it is running:
 * Ctrl-Break at a Windows console, Ctrl-\ at a POSIX one, and a hung-up
 * stdout, which is the far end leaving without saying so. Latched in a signal
 * handler or on another thread, so it only records the ask and whoever owns
 * the machine performs it. Asking twice is the hard way out, for a machine
 * too wedged to reach its own teardown. */
bool os_console_break_asked(void);
void os_console_break_ask(void);

/* Leave the way the ask asked, once the machine is down: the signal that was
 * sent, re-raised with its default handler, so a make or a shell loop hears a
 * run that was interrupted rather than one that merely failed. Where a host
 * has no such thing, its own default ends the process. Does not return. */
void os_console_break_exit(void);

#endif /* _OSAL_CONSOLE_H_ */
