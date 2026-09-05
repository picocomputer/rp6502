/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The desktop's standard streams: the program's stdout mirrored to the
 * host's, and under --headless the host's stdin fed to the program's.
 */

#ifndef _HOST_SOKOL_CLI_STREAMS_H_
#define _HOST_SOKOL_CLI_STREAMS_H_

#include <stdbool.h>

/* Mirror the program's stdout to the host's. Not under --script or --dap,
 * which own host stdout, nor --crc, whose value it is. */
void streams_mirror_stdout(void);

/* Put the host's stdio on the machine's console wire. True when the far end
 * is a terminal, which then is the console: keys raw, the machine's screen
 * drawn on it, and the internal terminal left a mirror that answers nothing.
 * A pipe or a file only feeds the wire. */
bool streams_console_open(void);

/* A headless run with nothing left to do: the program is parked on a read and
 * the wire is empty, so only the host can move it. Without this an unpaced
 * run spins the machine's clock forward over an empty wire. */
void streams_stdin_idle(void);

#endif /* _HOST_SOKOL_CLI_STREAMS_H_ */
