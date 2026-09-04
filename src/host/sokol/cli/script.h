/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _HOST_SOKOL_CLI_SCRIPT_H_
#define _HOST_SOKOL_CLI_SCRIPT_H_

#include <stdbool.h>
#include <stdio.h>

/* Scripted input: a line-oriented command language that plugs in gamepads,
 * moves pointers, types and asserts, so a program reading its input from XRAM
 * can be tested with nothing at the keyboard. This is a host in the same sense
 * as host/itch.io/webapi.c and host/sokol/android/window.c — it assembles
 * reports and hands them to the same hid seams a real device would.
 *
 * A failed assertion prints the script line and ends the run; script_exit_code is
 * what the process should return.
 *
 * `reply` turns on a line of stdout per command — ok, ok <values>, or
 * fail <why> — answered when the command finishes rather than when it parses.
 * Off until asked, so a driver writes a preamble without waiting for anything
 * and reads the ok for `reply` itself as the moment the machine starts
 * answering. */

/* Open a script and arm it. "-" reads stdin one line at a time, which is what
 * lets a driver in any language work the machine: the machine waits for each
 * line, so the driver is the clock. */
bool script_load(const char *path);

/* The verbs, for --help. Printed here rather than in cli.c so the list
 * cannot drift from what the parser accepts. */
void script_usage(FILE *out);

/* True when --script was given at all, and true while the script is still
 * going. A loaded script that stopped running is a finished run. */
bool script_loaded(void);
bool script_running(void);

/* Advance the script until it owes the machine a frame: settle whatever it is
 * waiting for, then run commands until the next one that has to wait. Returning
 * IS the request for a frame, so the caller runs exactly one and calls again —
 * which is what makes `run 600` six hundred frames and not "at least" six
 * hundred. The script is the only clock; nothing paces it against real time. */
void script_task(void);

/* Run one command line. False on a bad line or a failed assertion, either of
 * which ends the run. Public so a transport other than a file can drive the
 * same verbs. */
bool script_command(const char *line);

/* 0 when every assertion held. */
int script_exit_code(void);

#endif /* _HOST_SOKOL_CLI_SCRIPT_H_ */
