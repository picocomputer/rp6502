/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _HOST_SOKOL_CLI_SCRIPT_H_
#define _HOST_SOKOL_CLI_SCRIPT_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Scripted input: a line-oriented command language that plugs in gamepads,
 * moves pointers, types and asserts, so a program reading its input from XRAM
 * can be tested with nothing at the keyboard. Like the other hosts, it
 * assembles reports and hands them to the same core/hid entry points a real
 * device would.
 *
 * A failed assertion names the line it failed on and where the 6502 is, then
 * writes the program's stdout that no check used, and ends the run;
 * script_exit_code is what the process should return.
 *
 * `reply` turns on a line of stdout per command -- ok, ok <values>, or
 * fail <why> -- written when the command finishes rather than when it parses,
 * because a driver that read it sooner would race the machine. It stays off
 * until a script asks, so a driver can send a whole preamble without waiting
 * and then read the ok for `reply` itself as the moment the machine begins
 * answering.
 *
 * Any number can be a Python expression using the names in the ROM's .sym,
 * and `let name = expr` sets a variable that later lines can use. */

/* Open a script and arm it, with the names from the .sym beside rom. "-"
 * reads stdin one line at a time, so a driver in any language can work the
 * machine: the machine waits for each line. */
bool script_load(const char *path, const char *rom);

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

/* Make the sound of the frame that just ran. The caller runs it after every
 * frame, as a sink would pull it. */
void script_listen(void);

/* 0 when every assertion held. */
int script_exit_code(void);

/* How RAM and XRAM were filled, for status: one byte, or a random fill from
 * memseed. */
void script_set_memory(bool memfill_set, uint8_t memfill, uint32_t memseed);

#endif /* _HOST_SOKOL_CLI_SCRIPT_H_ */
