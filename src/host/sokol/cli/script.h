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
 * can be tested with nothing at the keyboard. Like the other hosts, it
 * assembles reports and hands them to the same core/hid entry points a real
 * device would.
 *
 * A failed assertion names the line it failed on and ends the run;
 * script_exit_code is what the process should return.
 *
 * `reply` turns on a line of stdout per command -- ok, ok <values>, or
 * fail <why> -- written when the command finishes rather than when it parses,
 * because a driver that read it sooner would race the machine. It stays off
 * until a script asks, so a driver can send a whole preamble without waiting
 * and then read the ok for `reply` itself as the moment the machine begins
 * answering. */

/* Open a script and arm it. "-" reads stdin one line at a time, so a driver
 * in any language can work the machine: the machine waits for each line. */
bool script_load(const char *path);

/* The verbs, for --help. Printed here rather than in cli.c because cli.c is
 * also linked without script.c. */
void script_usage(FILE *out);

/* Loaded is true once --script has opened one; running is true only while it
 * is still going, so a loaded script that is not running has finished. */
bool script_loaded(void);
bool script_running(void);

/* Advance the script until it owes the machine a frame: settle whatever it is
 * waiting for, then run commands until the next one that has to wait.
 * Returning is itself the request for a frame, so the caller runs exactly one
 * and calls again, which is what makes `run 600` six hundred frames rather
 * than at least six hundred. */
void script_task(void);

/* Run one command line. False on a bad line or a failed assertion, either of
 * which ends the run. */
bool script_command(const char *line);

/* 0 when every assertion held. */
int script_exit_code(void);

/* Watch the console the script is matching. com.c holds one console tap,
 * which the script takes, so this is the only way to see those bytes while a
 * script runs. */
void script_set_echo(void (*echo)(const char *buf, int len));

#endif /* _HOST_SOKOL_CLI_SCRIPT_H_ */
