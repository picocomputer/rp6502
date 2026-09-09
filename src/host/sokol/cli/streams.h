/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The program's stdout and stderr, mirrored onto the host's and converted
 * from OEM to UTF-8. The machine's console is cli/console.h and its own log
 * lines are cli/log.c; neither is a program's stream.
 */

#ifndef _HOST_SOKOL_CLI_STREAMS_H_
#define _HOST_SOKOL_CLI_STREAMS_H_

#include <stdbool.h>
#include <stdio.h>

/* Machine bytes onto one of the host's streams, as UTF-8. True when the bytes
 * ended a line, which is when they were flushed, because Windows has no line
 * buffering and nothing would arrive until something flushed. A write error is
 * left for the caller to find with ferror, since what it means differs by
 * stream. */
bool streams_write(FILE *f, const char *buf, int len);

/* Mirror the program's stdout to the host's. Not under --script, which owns
 * the host's stdout, nor --crc, whose value it prints there, nor where a
 * console terminal already carries those bytes to the same screen. */
void streams_mirror_stdout(void);

void streams_stderr(const char *buf, int len);

/* Install the above as the program's stderr. Every run does this, because no
 * mode of the emulator claims the host's stderr for itself. It is
 * cleared again where a console terminal already carries the same bytes to
 * the same screen. */
void streams_mirror_stderr(void);

#endif /* _HOST_SOKOL_CLI_STREAMS_H_ */
