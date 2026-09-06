/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The desktop's standard streams: which of the program's the host's carry,
 * and in what encoding. The machine's console is cli/console.h and the
 * machine's own lines are cli/log.c; neither is a program's stream.
 */

#ifndef _HOST_SOKOL_CLI_STREAMS_H_
#define _HOST_SOKOL_CLI_STREAMS_H_

#include <stdbool.h>
#include <stdio.h>

/* Machine bytes onto one of the host's streams, in the host's encoding. True
 * when a line ended, which is when it flushed -- Windows has no line
 * buffering, so nothing arrives until someone says so. Reports no error: what
 * a failed write means is the caller's, and it differs by stream. */
bool streams_write(FILE *f, const char *buf, int len);

/* Mirror the program's stdout to the host's. Not under --script or --dap,
 * which own host stdout, nor --crc, whose value it is. */
void streams_mirror_stdout(void);

/* Machine bytes on the host's stderr. Two things want this: the program's own
 * errors, and EMU_ECHO's copy of the whole console, which is how a run that
 * failed is read without rendering a frame. */
void streams_stderr(const char *buf, int len);

/* Install the above as the program's stderr. Every desktop run: unlike stdout,
 * no mode of the emulator claims host stderr for itself. Cleared where the
 * console terminal already carries the same stream to the same screen. */
void streams_mirror_stderr(void);

#endif /* _HOST_SOKOL_CLI_STREAMS_H_ */
