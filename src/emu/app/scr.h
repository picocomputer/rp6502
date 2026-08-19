/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_APP_SCR_H_
#define _EMU_APP_SCR_H_

#include <stdbool.h>

/* Scripted input: a line-oriented command language that plugs in gamepads,
 * moves pointers, types and asserts, so a program reading its input from XRAM
 * can be tested with nothing at the keyboard. This is a host in the same sense
 * as host/web/exports.c and host/android/window.c — it assembles reports and
 * hands them to the same hid seams a real device would.
 *
 * The verbs are listed in cli_usage(). A failed assertion prints the script
 * line and ends the run; scr_exit_code is what the process should return. */

/* Open a script and arm it. "-" reads stdin one line at a time, which is what
 * lets a driver in any language work the machine without a protocol: the
 * machine waits for each line, so the driver is the clock. */
bool scr_load(const char *path);

/* True when --script was given at all, and true while the script is still
 * going. A loaded script that stopped running is a finished run. */
bool scr_loaded(void);
bool scr_running(void);

/* Advance the script until it owes the machine a frame: settle whatever it is
 * waiting for, then run commands until the next one that has to wait. Returning
 * IS the request for a frame, so the caller runs exactly one and calls again —
 * which is what makes `run 600` six hundred frames and not "at least" six
 * hundred. The script is the only clock; nothing paces it against real time. */
void scr_task(void);

/* Run one command line. False on a bad line or a failed assertion, either of
 * which ends the run. Public so a transport other than a file can drive the
 * same verbs. */
bool scr_command(const char *line);

/* 0 when every assertion held. */
int scr_exit_code(void);

#endif /* _EMU_APP_SCR_H_ */
